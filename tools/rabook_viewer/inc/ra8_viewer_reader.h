/**
 * @file ra8_viewer_reader.h
 * @brief Host-side direct-call reader: open an e-book/comic and rasterise pages.
 *
 * @details
 * The desktop viewer links the firmware's platform-agnostic reader libraries
 * (compiled host-side with RA8_OFF_TARGET) and calls them directly -- there
 * is no ARM emulation. This module is the C core: it opens a file from disk
 * behind a seek+read callback and drives the comic (ra8_comic) and webtoon
 * (ra8_longstrip) engines. It exposes two render surfaces: a fixed RGB565
 * framebuffer (::ra8_viewer_render_page, for the headless dump) and a
 * continuous-scroll tile API (::ra8_viewer_render_tile565) the window scales to
 * width -- both built on ra8_reflow's `ra8_img_decode_blit` over ra8_gfx.
 *
 * The Objective-C window backend (ra8_viewer_view.h) consumes the tiles; the C
 * core shares nothing with it but plain buffers, so it stays testable and
 * dumpable without a display.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
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
 *          archives (`.cbz` / `.cbr` / `.cbt`, and gzip/xz-wrapped variants) open
 *          through ra8_comic and JOF webtoons (`.jof`) through ra8_longstrip;
 *          EPUB / RABOOK return ::k_ra8_err_not_supported for now (reflow render
 *          is a TODO seam -- see the .c file).
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
 * @brief Number of vertically-stacked tiles in the document.
 *
 * @details The desktop window reads the document as a continuous vertical strip
 *          of tiles, each rendered at native resolution and scaled to the window
 *          width by the view. For comics one tile == one page; for a JOF
 *          webtoon the tall strip is split into framebuffer-height bands. This is
 *          the same count as ::ra8_viewer_page_count, named for the scroll model.
 * @param[in] r Reader (may be NULL).
 * @return Tile count, or 0 for a NULL reader.
 * @since 0.1.0
 */
[[nodiscard]] uint32_t ra8_viewer_tile_count(const ra8_viewer_reader_t* r);

/**
 * @brief Native pixel dimensions of tile @p i (for laying out the scroll canvas).
 * @param[in]  r Reader (non-NULL).
 * @param[in]  i Tile index (`< ra8_viewer_tile_count(r)`).
 * @param[out] w Receives the tile's native width in pixels.
 * @param[out] h Receives the tile's native height in pixels.
 * @return ra8_err_t; ::k_ra8_err_out_of_range if @p i is past the tile count.
 * @note Sizes are cached at open, so this is cheap to call for every tile.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_viewer_tile_size(const ra8_viewer_reader_t* r, uint32_t i, uint32_t* w, uint32_t* h);

/**
 * @brief Render tile @p i at native resolution into a fresh RGB565 buffer.
 *
 * @details Allocates a `w*h` RGB565 buffer, rasterises tile @p i into it via the
 *          document's engine (comic page decode, or one webtoon band), and
 *          transfers ownership to the caller (who must `free` it). Unlike
 *          ::ra8_viewer_render_page this does not touch the shared framebuffer,
 *          so window tiles and the headless framebuffer path never interfere.
 *
 * @param[in,out] r   Reader (non-NULL).
 * @param[in]     i   Tile index (`< ra8_viewer_tile_count(r)`).
 * @param[out]    w   Receives the rendered width in pixels.
 * @param[out]    h   Receives the rendered height in pixels.
 * @param[out]    out Receives a malloc'd `w*h` RGB565 buffer (caller frees).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Tile rendered; `*out` owns `w*h` pixels.
 * @retval k_ra8_err_null_ptr     A required pointer was NULL.
 * @retval k_ra8_err_out_of_range @p i is at or past the tile count.
 * @retval k_ra8_err_no_mem       The output buffer could not be allocated.
 * @retval k_ra8_err_*            An engine / decode error (e.g. a page too tall
 *                                for the whole-image decoder); `*out` stays NULL.
 * @post On any error `*out` is left NULL and nothing is leaked.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_viewer_render_tile565(ra8_viewer_reader_t* r,
                                                  uint32_t             i,
                                                  uint32_t*            w,
                                                  uint32_t*            h,
                                                  uint16_t**           out);

/**
 * @brief Write the current framebuffer to a binary PPM (P6) file.
 * @details A display-free rendering proof: the RGB565 framebuffer is expanded to
 *          8-bit RGB and written as a P6 portable pixmap, so a page can be dumped
 *          on a headless host and converted to PNG offline. A thin wrapper over
 *          ::ra8_viewer_write_ppm565 bound to the reader's fixed framebuffer.
 * @param[in] r    Reader whose framebuffer to dump (non-NULL).
 * @param[in] path Output PPM path (non-NULL).
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok            File written.
 * @retval k_ra8_err_null_ptr  @p r or @p path was NULL.
 * @retval k_ra8_err_not_found The output path could not be opened for writing.
 * @see ra8_viewer_write_ppm565()  Dump an arbitrary RGB565 buffer (scroll tiles).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_viewer_dump_ppm(const ra8_viewer_reader_t* r, const char* path);

/**
 * @brief Write an arbitrary RGB565 buffer to a binary PPM (P6) file.
 *
 * @details Expands each 16-bit pixel to 8-bit RGB by replicating the high bits
 *          into the low bits, so a saturated field maps to 0xFF exactly rather
 *          than 0xF8. Shared by the fixed-framebuffer dump and the scroll-tile
 *          dump so the two cannot drift apart.
 *
 * @param[in] px   Packed RGB565 pixels, row-major, `w * h` entries (non-NULL).
 * @param[in] w    Image width in pixels (>= 1).
 * @param[in] h    Image height in pixels (>= 1).
 * @param[in] path Output PPM path (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               File written and closed.
 * @retval k_ra8_err_null_ptr     @p px or @p path was NULL.
 * @retval k_ra8_err_invalid_size @p w or @p h was 0.
 * @retval k_ra8_err_not_found    The path could not be opened, or close failed.
 *
 * @post On ::k_ra8_ok the file holds a complete `w * h` P6 pixmap.
 * @note Not thread-safe with respect to the same output path.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_viewer_write_ppm565(const uint16_t* px, uint32_t w, uint32_t h, const char* path);

/**
 * @brief Pack 8-bit RGB channels into a little-endian RGB565 word.
 * @param[in] rr Red channel (0-255).
 * @param[in] gg Green channel (0-255).
 * @param[in] bb Blue channel (0-255).
 * @return The packed RGB565 value.
 * @note Pure function; safe from any context.
 * @see ra8_viewer_pack565_le_pair()  Reassemble an already-packed pixel.
 * @since 0.1.0
 */
[[nodiscard]] uint16_t ra8_viewer_pack565(uint8_t rr, uint8_t gg, uint8_t bb);

/**
 * @brief Reassemble a little-endian RGB565 word from its two source bytes.
 * @details Used when a long-strip band is already stored as RGB565 and only
 *          needs re-forming from the byte stream, not re-quantising.
 * @param[in] lo Low byte of the pixel.
 * @param[in] hi High byte of the pixel.
 * @return The packed RGB565 value.
 * @note Pure function; safe from any context.
 * @since 0.1.0
 */
[[nodiscard]] uint16_t ra8_viewer_pack565_le_pair(uint8_t lo, uint8_t hi);

/**
 * @brief Release a reader and all of its owned buffers.
 * @details Frees the format-specific backing (comic archive or JOF cache), the
 *          framebuffer and the reader struct itself. Safe to call with NULL,
 *          which is a no-op.
 * @param[in,out] r Reader from ::ra8_viewer_open (NULL is ignored).
 * @pre @p r came from ::ra8_viewer_open, or is NULL.
 * @pre @p r is not used again after this call.
 * @post Every buffer the reader owned is freed and @p r is invalid.
 * @post No resource borrowed from outside the reader is touched.
 * @note Not thread-safe (mutates and frees the reader).
 * @since 0.1.0
 */
void ra8_viewer_close(ra8_viewer_reader_t* r);

#ifdef __cplusplus
}
#endif
