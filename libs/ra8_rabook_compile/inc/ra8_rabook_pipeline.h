/**
 * @file ra8_rabook_pipeline.h
 * @brief End-to-end EPUB -> RABOOK1 compile pipeline (#149).
 * @ingroup grp_ereader
 *
 * @details
 * Wires the back-end stages into a single call, in the desktop epub_compile.py
 * emit order so the RABOOK1 blob is byte-identical:
 *
 *  1. Stylesheets -- every `text/css` manifest item, loaded via
 *     @ref ra8_epub_get_resource and interned, in OPF order.
 *  2. Images -- every image manifest item in OPF order: `image/svg+xml` stored
 *     verbatim, other `image/` types decoded to 8-bit grayscale via stb_image,
 *     downscaled + quantised to 4-bpp (@ref ra8_rabook_gray4_downscale /
 *     @ref ra8_rabook_gray4_encode), and stored via @ref ra8_rabook_add_image. The
 *     cover index is resolved from the cover manifest item.
 *  3. Spine chapters -- raw XHTML extracted by @ref ra8_epub_load_chapter,
 *     parsed and DOM-built by @ref ra8_rabook_xml_parse_chapter.
 *  4. Metadata -- Dublin Core from the open @ref ra8_epub_book_t, interned LAST
 *     and recorded via @ref ra8_rabook_set_metadata.
 *
 * All working storage is caller-supplied (@ref ra8_rabook_pipeline_scratch_t);
 * no heap is touched (the stb_image arena is backed by the caller's
 * @p img_arena).  The caller is responsible for opening and closing the
 * @ref ra8_epub_book_t and for providing correctly-sized arenas.
 *
 * @par NASA Rule 3 (no heap)
 * stb_image is the sole allocation source; it is redirected to
 * @ref ra8_img_arena_t -- a caller-owned bump arena. XML parsing uses the
 * explicit caller-owned @ref ra8_rabook_xml_workspace_t.
 *
 * @note Not thread-safe.
 * @see ra8_rabook_compile.h   Builder API (emitter).
 * @see ra8_rabook_gray4.h     Image transcode stage.
 * @see ra8_rabook_xml_shim.h  XHTML -> DOM stage.
 * @see ra8_epub.h             EPUB reader the pipeline drives.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 *
 * [Ring 4 / EPUB Compiler] {World: NS}
 *
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_epub.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_rabook_compile.h"
#include "ra8_rabook_xml_shim.h"
#include "ra8_reflow_image.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @struct ra8_rabook_pipeline_scratch_t
 * @brief Caller-owned temporary buffers the pipeline stage needs.
 *
 * @details All buffer fields must be set to valid, non-NULL pointers with the
 *          capacities large enough for the book being compiled.  The pipeline
 *          never allocates; it writes only into these buffers and the builder
 *          arenas inside @ref ra8_rabook_buffers_t.
 *
 *          @p skip_images (default false): when true the image stage is a no-op
 *          -- no image-table entries are added and the cover index stays nil, so
 *          the blob is text/CSS-only. It is the on-device equivalent of the
 *          desktop epub_compile.py @c --no-images option (its empty
 *          @c manifest.items() image loop), and yields a blob byte-identical to
 *          that desktop @c --no-images golden. Stylesheets, chapters and metadata
 *          are unaffected. The @p image_raw / @p img_arena / @p gray buffers may
 *          stay tiny when @p skip_images is true.
 *
 *          @p pixel_format (default 0 == @ref k_ra8_book_pixfmt_gray4) is the
 *          device profile that selects the raster depth: 4-bpp packed grayscale
 *          for an even 16-level e-ink panel, or @ref k_ra8_book_pixfmt_gray8 for
 *          the lossless 8-bpp source on a deeper panel. It only affects raster
 *          transcode; SVG, stylesheets, chapters and metadata are unaffected.
 *
 * @invariant Every pointer is non-NULL and each buffer holds at least its cap.
 * @invariant @p xhtml, @p image_raw, @p gray and the @p img_arena backing store
 *            do not overlap. The transcode stage reuses @p image_raw in place as
 *            the encode output (4-bpp nibbles or the 8-bpp copy) while the decoded
 *            source pixels are still live in @p img_arena (and @p gray on the
 *            downscale path), so an aliased buffer would corrupt the still-live
 *            source.
 * @warning Do not alias these buffers: @p image_raw is overwritten with the
 *          encoded output while @p img_arena / @p gray still hold the pixels
 *          being read.
 * @code
 *   static uint8_t xhtml_buf[64U * 1024U];
 *   static uint8_t image_raw[4U * 1024U * 1024U];
 *   static uint8_t img_scratch[4U * 1024U * 1024U];
 *   static uint8_t gray_buf[1600U * 1200U];
 *   ra8_img_arena_t arena = { img_scratch, sizeof(img_scratch), 0U, 0U };
 *   ra8_rabook_pipeline_scratch_t scr = {
 *       xhtml_buf, sizeof(xhtml_buf),
 *       image_raw, sizeof(image_raw),
 *       &arena,
 *       gray_buf, sizeof(gray_buf),
 *   };
 * @endcode
 * @see ra8_rabook_compile_from_epub
 * @since Version 0.1.0
 */
typedef struct {
  uint8_t*                    xhtml;          /**< Chapter XHTML scratch buffer.                */
  size_t                      xhtml_cap;      /**< Capacity of @p xhtml in bytes.               */
  uint8_t*                    image_raw;      /**< Raw encoded cover/image byte buffer.         */
  size_t                      image_cap;      /**< Capacity of @p image_raw in bytes.           */
  ra8_img_arena_t*            img_arena;      /**< stb_image bump arena (caller-sized scratch). */
  uint8_t*                    gray;           /**< Intermediate gray-pixel downscale buffer.    */
  uint32_t                    gray_cap;       /**< Capacity of @p gray in pixels (bytes).       */
  char*                       css;            /**< Stylesheet load scratch (NUL-terminated).    */
  size_t                      css_cap;        /**< Capacity of @p css in bytes.                 */
  bool                        skip_images;    /**< Drop all images (text/CSS-only). See below.  */
  uint16_t                    max_image_edge; /**< Opt-in downscale clamp on the longer edge in
                                   *   pixels; 0 (the zero-init default) preserves
                                   *   source resolution (full-res manga for the
                                   *   zoom loupe). Mirrors the desktop tool's
                                   *   opt-in `--max-edge` knob.                  */
  uint8_t                     pixel_format;   /**< Device profile: raster depth to emit,
                                   *   @ref ra8_book_image_pixfmt_t. 0 (the
                                   *   zero-init default) is
                                   *   @ref k_ra8_book_pixfmt_gray4 -- the 4-bpp
                                   *   packing a grayscale panel wants;
                                   *   @ref k_ra8_book_pixfmt_gray8 keeps the
                                   *   lossless 8-bpp source for a deeper panel. */
  ra8_rabook_xml_workspace_t* xml_workspace;  /**< Caller-owned XHTML parser state. */
} ra8_rabook_pipeline_scratch_t;

/**
 * @brief Compile an open EPUB into a RABOOK1 blob written to @p out_path on
 *        the SD filesystem.
 *
 * @details
 * Full pipeline, in order:
 *
 * The stage order mirrors the desktop epub_compile.py so the emitted blob is
 * byte-identical: stylesheets, then images (cover), then chapters, then
 * metadata is interned last.
 *
 *  -# Initialise the builder context via @ref ra8_rabook_compile_init.
 *  -# For each `text/css` manifest item (OPF order): load it into @p scratch->css
 *     and add it via @ref ra8_rabook_add_stylesheet.
 *  -# If a cover image is present: extract raw bytes, decode to 8-bit grey
 *     with stb_image (using @p scratch->img_arena), downscale to at most
 *     the caller's opt-in `max_image_edge` clamp (source resolution when the
 *     field is 0, the default) on the longer edge, encode to 4-bpp,
 *     and add via @ref ra8_rabook_add_image.
 *  -# For each spine chapter (0..chapter_count): extract the raw XHTML into
 *     @p scratch->xhtml, look up the matching TOC entry for a title, and call
 *     @ref ra8_rabook_xml_parse_chapter.
 *  -# Read Dublin Core metadata from @p epub and call
 *     @ref ra8_rabook_set_metadata (interned after the chapters).
 *  -# Finalise the blob via @ref ra8_rabook_finalize.
 *  -# Write the blob to @p out_path via @ref ra8_fs_write_file.
 *
 * @param[in,out] epub    Open book (in_use == 1); chapters are extracted
 *                        in-place from the ZIP.
 * @param[in]     buffers Builder arenas (all non-NULL, sized for the book).
 * @param[in]     scratch Scratch buffers for XHTML load + image decode + gray.
 * @param[in,out] mount   Mounted filesystem volume to write @p out_path onto.
 * @param[in]     out_path Filesystem path of the output .rabook file.
 *
 * @return Error code.
 * @retval k_ra8_ok               Book compiled and written to @p out_path.
 * @retval k_ra8_err_null_ptr     Any required pointer is NULL.
 * @retval k_ra8_err_no_mem       An arena overflowed, a scratch buffer was too
 *                               small, or a present cover failed to transcode.
 * @retval k_ra8_err_not_found    A chapter ZIP entry referenced by the spine
 *                               is missing.
 * @retval k_ra8_err_invalid_size The laid-out blob exceeds the output arena
 *                               (propagated from @ref ra8_rabook_finalize).
 * @return Any other error code is propagated unchanged from the EPUB reader
 *         (@ref ra8_epub_get_metadata / @ref ra8_epub_load_chapter / ...) or from
 *         the filesystem write (@ref ra8_fs_write_file).
 *
 * @pre @p epub->in_use == 1.
 * @pre All arenas and scratch buffers are non-NULL with valid capacities.
 * @post On k_ra8_ok, @p out_path contains a valid RABOOK1 blob.
 * @post On failure, partial output may exist at @p out_path.
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
ra8_err_t ra8_rabook_compile_from_epub(ra8_epub_book_t*                     epub,
                                       const ra8_rabook_buffers_t*          buffers,
                                       const ra8_rabook_pipeline_scratch_t* scratch,
                                       ra8_fs_mount_t*                      mount,
                                       const char*                          out_path);

/**
 * @brief Compile an open EPUB into a RABOOK1 blob left in @p bufs->out, with no
 *        filesystem write.
 *
 * @details
 * Identical compile to @ref ra8_rabook_compile_from_epub -- same stages, same
 * byte-identical desktop emit order -- but it stops at @ref ra8_rabook_finalize
 * and returns the blob in place instead of calling @ref ra8_fs_write_file. This is
 * the entry point for callers with no filesystem: notably the Cortex-M33 offload
 * (#149), which finalises into a shared SDRAM buffer and lets the M85 own the SD
 * write. The returned @p *out_blob aliases @p bufs->out and is valid only while
 * that arena is. A build defining @c RA8_RABOOK_NO_RASTER (the M33 text/CSS/SVG
 * image) links no stb_image: raster manifest images are skipped, SVG verbatim and
 * text/CSS are unaffected.
 *
 * @param[in,out] epub     Open book (in_use == 1); chapters extracted in-place.
 * @param[in]     bufs     Builder arenas (all non-NULL, sized for the book).
 * @param[in]     scr      Scratch buffers for XHTML load + image decode + gray.
 * @param[out]    out_blob Receives a pointer to the blob inside @p bufs->out.
 * @param[out]    out_len  Receives the blob length in bytes.
 *
 * @return Error code.
 * @retval k_ra8_ok           Book compiled; @p *out_blob / @p *out_len set.
 * @retval k_ra8_err_null_ptr Any required pointer is NULL.
 * @return Any other code is propagated from a compile stage or
 *         @ref ra8_rabook_finalize (see @ref ra8_rabook_compile_from_epub).
 *
 * @pre @p epub->in_use == 1.
 * @pre All arenas and scratch buffers are non-NULL with valid capacities.
 * @post On k_ra8_ok @p *out_blob addresses a valid RABOOK1 blob in @p bufs->out.
 * @post No filesystem state is touched.
 *
 * @note Not thread-safe.
 * @see ra8_rabook_compile_from_epub  The filesystem-writing variant.
 * @since Version 0.1.0
 */
ra8_err_t ra8_rabook_compile_from_epub_to_buffer(ra8_epub_book_t*                     epub,
                                                 const ra8_rabook_buffers_t*          bufs,
                                                 const ra8_rabook_pipeline_scratch_t* scr,
                                                 const void**                         out_blob,
                                                 uint32_t*                            out_len);

#ifdef __cplusplus
} /* extern "C" */
#endif
