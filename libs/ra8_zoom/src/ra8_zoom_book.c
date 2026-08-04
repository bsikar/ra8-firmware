/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_zoom_book.c
 * @brief `.rabook` image-pool source adapter for the tap-to-zoom viewer (#478).
 *
 * @details Implements ra8_zoom_book.h. Deliberately thin: the image-pool
 *          addressing contract, the gray4 nibble parity and the paged
 *          frame-by-frame reads all belong to ::ra8_book_src_image_rect and are
 *          not re-implemented here. This file only decides whether an entry is
 *          magnifiable at all and presents the existing call as the engine's
 *          gray8 sub-rect seam.
 *
 *          The whole TU is guarded on `ra8_book_paged.h` being reachable, the
 *          same pattern `ra8_comic_tiles.c` uses, so an app that magnifies only
 *          tiled comics does not link `ra8_book`.
 *
 *
 * [Ring 4 / Domain] {World: NS}
 *
 * @since 0.1.0
 */

#ifdef __has_include
#if __has_include("ra8_book_paged.h")
/**
 * @def RA8_ZOOM_HAVE_BOOK
 * @brief Defined when `ra8_book` is on the include path, enabling this adapter.
 *
 * @details A build-configuration flag, not a constant: it carries no value and
 *          is only ever tested with `#ifdef`. When it is absent the whole
 *          translation unit compiles to nothing, so an app that magnifies only
 *          tiled comics never has to link `ra8_book` to satisfy a symbol it
 *          does not call. The same pattern guards `ra8_comic_tiles.c`.
 *
 * @note Set by this file alone; never define it externally to force the
 *       adapter in -- the include would then fail instead.
 * @warning Undefining it silently removes ::ra8_zoom_book_src_init and friends
 *          from the link, which presents as an undefined-symbol error at the
 *          call site rather than here.
 *
 * @par Example:
 * @code
 * #ifdef RA8_ZOOM_HAVE_BOOK
 * // ... the adapter's whole implementation ...
 * #endif
 * @endcode
 *
 * @see ra8_zoom_book.h
 * @since 0.1.0
 */
#define RA8_ZOOM_HAVE_BOOK
#endif
#endif

#ifdef RA8_ZOOM_HAVE_BOOK

#include "ra8_zoom_book.h"

#include <stdint.h>

#include "ra8_book.h"
#include "ra8_book_paged.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_zoom.h"

/** @brief Component tag for `RA8_CHECK_*` log lines. */
static const char* const s_tag = "ra8_zoom_book";

ra8_err_t
ra8_zoom_book_src_init(ra8_zoom_book_src_t* bs, const ra8_book_src_t* src, uint32_t img_idx)
{
  RA8_CHECK_NULL_PTR(bs, s_tag, "binding must not be nullptr");
  RA8_CHECK_NULL_PTR(src, s_tag, "book source must not be nullptr");
  ra8_book_image_t img = {};
  RA8_RETURN_ON_ERROR(ra8_book_src_image(src, img_idx, &img), s_tag, "image descriptor read");
  if (img.format != (uint8_t)k_ra8_book_image_gray4) {
    ra8_log_error(s_tag, "vector images are not magnifiable by the raster viewer");
    return k_ra8_err_not_supported;
  }
  /* Decision: a raster with no extent cannot be magnified (2 conditions). */
  if ((img.width == 0U) || (img.height == 0U)) {
    ra8_log_error(s_tag, "image descriptor has a zero extent");
    return k_ra8_err_invalid_arg;
  }
  bs->src = src;
  bs->img = img;
  return k_ra8_ok;
}

ra8_err_t ra8_zoom_book_src_bind(ra8_zoom_book_src_t* bs, ra8_zoom_source_t* out)
{
  RA8_CHECK_NULL_PTR(bs, s_tag, "binding must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "source out must not be nullptr");
  RA8_CHECK_NULL_PTR(bs->src, s_tag, "binding was never initialised");
  return ra8_zoom_source_init(out,
                              ra8_zoom_book_read,
                              bs,
                              (uint32_t)bs->img.width,
                              (uint32_t)bs->img.height);
}

ra8_err_t ra8_zoom_book_read(void*    ctx,
                             uint32_t x,
                             uint32_t y,
                             uint32_t w,
                             uint32_t h,
                             uint8_t* out,
                             uint32_t out_stride)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "read ctx must not be nullptr");
  RA8_CHECK_NULL_PTR(out, s_tag, "read destination must not be nullptr");
  const ra8_zoom_book_src_t* bs = (const ra8_zoom_book_src_t*)ctx;
  RA8_CHECK_NULL_PTR(bs->src, s_tag, "binding was never initialised");
  return ra8_book_src_image_rect(bs->src, &bs->img, x, y, w, h, out, out_stride);
}

#endif /* RA8_ZOOM_HAVE_BOOK */
