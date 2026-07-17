/**
 * @file ra8_viewer_reader.h
 * @brief Host-side direct-call reader: open an e-book/comic and rasterise pages.
 *
 * @details
 * The desktop viewer links the firmware's platform-agnostic reader libraries
 * (compiled host-side with RA8_SIMULATOR_MODE) and calls them directly -- there
 * is no ARM emulation. This module is the C core: it opens a file from disk
 * behind a seek+read callback, drives the comic engine (ra8_comic) to page
 * through a `.cbz` / `.cbr` archive, and rasterises one page at a time into an
 * owned RGB565 framebuffer via ra8_reflow's `ra8_img_decode_blit` over ra8_gfx.
 *
 * The Objective-C window backend (ra8_viewer_view.h) presents that framebuffer;
 * the two halves share nothing but the RGB565 buffer, so the reader is testable
 * and dumpable without a display.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */
#pragma once

#include <stdint.h>

#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum ra8_viewer_geom_t
 * @brief Fixed framebuffer geometry the viewer rasterises pages into.
 * @details A portrait 2:3 page canvas; each page image is scaled to fit while
 *          preserving aspect ratio and centred, so the surrounding margin stays
 *          the framebuffer's background colour.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_viewer_fb_width  = 720U,  /**< Framebuffer width in pixels.  */
  k_ra8_viewer_fb_height = 1080U, /**< Framebuffer height in pixels. */
} ra8_viewer_geom_t;

/**
 * @struct ra8_viewer_reader
 * @brief Opaque handle to one open document and its owned rasterisation buffers.
 * @details Allocated by ::ra8_viewer_open, released by ::ra8_viewer_close. The
 *          full definition lives in ra8_viewer_reader.c so callers depend only on
 *          the API below.
 * @since 0.1.0
 */
typedef struct ra8_viewer_reader ra8_viewer_reader_t;

/**
 * @brief Open a document from disk and prepare it for paged rasterisation.
 *
 * @details Detects the format by file extension and container magic. Comic
 *          archives (`.cbz` / `.cbr` / `.cbt`) are opened through the ra8_comic
 *          engine; other formats return ::k_ra8_err_not_supported for now (EPUB
 *          and webtoon are TODO seams -- see the .c file).
 *
 * @param[out] out  Receives the newly allocated reader handle on success.
 * @param[in]  path NUL-terminated filesystem path to the document.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Document opened; `*out` is valid.
 * @retval k_ra8_err_null_ptr      @p out or @p path was NULL.
 * @retval k_ra8_err_not_found     The file could not be opened or is empty.
 * @retval k_ra8_err_no_mem        A reader buffer allocation failed.
 * @retval k_ra8_err_not_supported The format is not (yet) handled.
 * @retval k_ra8_err_*             A reader-engine error.
 *
 * @post On any error `*out` is left NULL and nothing is leaked.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_viewer_open(ra8_viewer_reader_t** out, const char* path);

/**
 * @brief Number of pages in the open document.
 * @param[in] r Reader from ::ra8_viewer_open (may be NULL).
 * @return Page count, or 0 for a NULL reader.
 * @since 0.1.0
 */
[[nodiscard]] uint32_t ra8_viewer_page_count(const ra8_viewer_reader_t* r);

/**
 * @brief Rasterise one page into the reader's owned framebuffer.
 *
 * @details Clears the framebuffer to the background colour, extracts the page's
 *          encoded image bytes through the engine, then decodes and blits them
 *          scaled-to-fit and centred via `ra8_img_decode_blit`.
 *
 * @param[in,out] r    Reader from ::ra8_viewer_open (non-NULL).
 * @param[in]     page Page index (`< ra8_viewer_page_count(r)`).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Page rasterised into the framebuffer.
 * @retval k_ra8_err_null_ptr     @p r was NULL.
 * @retval k_ra8_err_out_of_range @p page is at or past the page count.
 * @retval k_ra8_err_no_mem       The page buffer or decode arena was too small.
 * @retval k_ra8_err_*            An engine / decode error.
 *
 * @post On ::k_ra8_ok the framebuffer holds the rendered page.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_viewer_render_page(ra8_viewer_reader_t* r, uint32_t page);

/**
 * @brief Borrow the reader's RGB565 framebuffer (little-endian, row-major).
 * @param[in] r Reader from ::ra8_viewer_open (non-NULL).
 * @return Pointer to `width * height` RGB565 pixels, or NULL if @p r is NULL.
 * @note The buffer is owned by @p r and valid until ::ra8_viewer_close.
 * @since 0.1.0
 */
[[nodiscard]] const uint16_t* ra8_viewer_framebuffer(const ra8_viewer_reader_t* r);

/**
 * @brief Framebuffer width in pixels.
 * @param[in] r Reader (may be NULL).
 * @return Width, or 0 for a NULL reader.
 * @since 0.1.0
 */
[[nodiscard]] uint16_t ra8_viewer_fb_width(const ra8_viewer_reader_t* r);

/**
 * @brief Framebuffer height in pixels.
 * @param[in] r Reader (may be NULL).
 * @return Height, or 0 for a NULL reader.
 * @since 0.1.0
 */
[[nodiscard]] uint16_t ra8_viewer_fb_height(const ra8_viewer_reader_t* r);

/**
 * @brief Write the current framebuffer to a binary PPM (P6) file.
 * @details A display-free rendering proof: the RGB565 framebuffer is expanded to
 *          8-bit RGB and written as a P6 portable pixmap, so a page can be dumped
 *          on a headless host and converted to PNG offline.
 * @param[in] r    Reader whose framebuffer to dump (non-NULL).
 * @param[in] path Output PPM path (non-NULL).
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok            File written.
 * @retval k_ra8_err_null_ptr  @p r or @p path was NULL.
 * @retval k_ra8_err_not_found The output path could not be opened for writing.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_viewer_dump_ppm(const ra8_viewer_reader_t* r, const char* path);

/**
 * @brief Release a reader and all of its owned buffers.
 * @param[in,out] r Reader from ::ra8_viewer_open (NULL is ignored).
 * @since 0.1.0
 */
void ra8_viewer_close(ra8_viewer_reader_t* r);

#ifdef __cplusplus
}
#endif
