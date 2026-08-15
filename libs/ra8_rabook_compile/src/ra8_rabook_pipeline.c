/**
 * @file ra8_rabook_pipeline.c
 * @brief End-to-end EPUB -> RABOOK1 compile pipeline (#149).
 * @details Coordinates EPUB parsing, chapter compilation, image conversion,
 * and final RABOOK publication through explicit caller-owned workspaces.
 * @since Version 0.1.0
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_rabook_pipeline.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_book.h"
#include "ra8_check.h"
#include "ra8_epub.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_img_arena.h"
#include "ra8_log.h"
#include "ra8_rabook_compile.h"
#include "ra8_rabook_gray4.h"
#include "ra8_rabook_xml_shim.h"
#include "ra8_reflow_image.h"
#include "stb_image.h"

/* -------------------------------------------------------------------------- */
/* Private constants */
/* -------------------------------------------------------------------------- */

/**
 * @enum ra8_pipeline_limits_t
 * @brief Static bounds used for NASA Rule 2 loop annotations.
 * @since Version 0.1.0
 */
typedef enum : uint16_t {
  k_pipeline_max_chapters = 64U, /**< == k_ra8_epub_max_chapters (cross-check). */
  k_pipeline_max_toc      = 64U, /**< == k_ra8_epub_max_toc (cross-check).      */
} ra8_pipeline_limits_t;

static_assert((uint16_t)k_pipeline_max_chapters == (uint16_t)k_ra8_epub_max_chapters,
              "pipeline chapter cap must match epub cap");
static_assert((uint16_t)k_pipeline_max_toc == (uint16_t)k_ra8_epub_max_toc,
              "pipeline toc cap must match epub cap");

/**
 * @enum ra8_pipeline_stbi_t
 * @brief stb_image channel-count selector (no magic literals).
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_stbi_grey = 1U, /**< Request 8-bit grayscale output from stb_image. */
} ra8_pipeline_stbi_t;

static const char* const s_tag = "ra8_rabook_pipeline";

/* -------------------------------------------------------------------------- */
/* Private helpers */
/* -------------------------------------------------------------------------- */

/* The raster image path (stb_image decode + gray4 transcode) is compiled out of a
 * RA8_RABOOK_NO_RASTER build -- e.g. the Cortex-M33 text/CSS/SVG-only image, which
 * then links no stb_image. SVG images are stored verbatim and need none of this. */
#ifndef RA8_RABOOK_NO_RASTER

/**
 * @brief Resolve the gray source buffer for the encode, downscaling if required.
 * @details When the target dimensions equal the source the decoded @p pixels are
 *          used in place; otherwise the pixels are bilinear-downscaled into
 *          @p scr->gray (after a capacity check) and that buffer is returned.
 * @param[in] scr    Scratch buffers (provides @p gray / @p gray_cap), non-NULL.
 * @param[in] pixels Decoded 8-bit grayscale source pixels, non-NULL.
 * @param[in] sw     Source width in pixels (> 0).
 * @param[in] sh     Source height in pixels (> 0).
 * @param[in] ow     Target output width in pixels (> 0).
 * @param[in] oh     Target output height in pixels (> 0).
 * @return Pointer to the gray pixels to encode (@p pixels or @p scr->gray), or
 *         nullptr if the gray scratch is too small or the downscale failed.
 * @retval nullptr Gray scratch capacity exceeded or downscale error.
 * @pre @p scr and @p pixels are non-NULL (caller-validated).
 * @pre @p ow * @p oh does not exceed the encode/gray scratch the caller sized.
 * @post On success the returned buffer holds @p ow * @p oh gray bytes.
 * @post @p pixels is unchanged unless it is itself the returned buffer.
 * @note Does not bind/unbind the image arena; the caller owns that lifetime.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static const uint8_t* internal_downscale_if_needed(const ra8_rabook_pipeline_scratch_t* scr,
                                                   const uint8_t*                       pixels,
                                                   uint16_t                             sw,
                                                   uint16_t                             sh,
                                                   uint16_t                             ow,
                                                   uint16_t                             oh)
{
  if (ow == sw && oh == sh) {
    return pixels;
  }
  if ((uint32_t)ow * oh > scr->gray_cap) {
    ra8_log_error(s_tag, "gray scratch too small for downscale");
    return nullptr;
  }
  if (ra8_rabook_gray4_downscale(pixels, sw, sh, scr->gray, ow, oh) != k_ra8_ok) {
    return nullptr;
  }
  return scr->gray;
}

/**
 * @brief Encode @p gray_src into @p scr->image_raw at the device-profile depth.
 * @details Dispatches on @p scr->pixel_format: @ref k_ra8_book_pixfmt_gray8 copies
 *          the pixels out verbatim at 8-bpp (1 byte/pixel), any other value
 *          quantises + nibble-packs to 4-bpp (2 pixels/byte). The output is written
 *          into @p scr->image_raw in place, so it must not overlap the still-live
 *          @p gray_src (see @ref ra8_rabook_pipeline_scratch_t).
 * @param[in]  scr      Scratch buffers (provides @p image_raw / @p image_cap /
 *                      @p pixel_format), non-NULL.
 * @param[in]  gray_src Gray pixels to encode: @p ow * @p oh readable bytes, non-NULL.
 * @param[in]  ow       Output width in pixels.
 * @param[in]  oh       Output height in pixels.
 * @param[out] out_size Receives the encoded byte length written to @p image_raw.
 * @return Error code from the selected encoder.
 * @retval k_ra8_ok         Encoded; @p *out_size bytes written to @p image_raw.
 * @retval k_ra8_err_no_mem @p image_cap is too small for the encoded output.
 * @pre @p scr, @p gray_src and @p out_size are non-NULL (caller-validated).
 * @pre @p gray_src holds at least @p ow * @p oh readable bytes.
 * @post On k_ra8_ok @p scr->image_raw holds @p *out_size encoded bytes.
 * @post @p gray_src is not modified (read-only encode).
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_encode_gray(const ra8_rabook_pipeline_scratch_t* scr,
                                      const uint8_t*                       gray_src,
                                      uint16_t                             ow,
                                      uint16_t                             oh,
                                      uint32_t*                            out_size)
{
  if (scr->pixel_format == (uint8_t)k_ra8_book_pixfmt_gray8) {
    return ra8_rabook_gray8_encode(gray_src,
                                   ow,
                                   oh,
                                   scr->image_raw,
                                   (uint32_t)scr->image_cap,
                                   out_size);
  }
  return ra8_rabook_gray4_encode(gray_src,
                                 ow,
                                 oh,
                                 scr->image_raw,
                                 (uint32_t)scr->image_cap,
                                 out_size);
}

/**
 * @brief Decode one raw image, transcode to the profile's gray depth, add to builder.
 * @details Binds @p scr->img_arena before calling stb_image and unbinds on every
 *          return path.  The encoded output (4-bpp nibbles or the 8-bpp copy, per
 *          @p scr->pixel_format) is written into @p scr->image_raw (reusing the
 *          buffer that held the raw bytes), so the source and encode buffers must
 *          not overlap (see @ref ra8_rabook_pipeline_scratch_t).
 * @param[in,out] ctx     Builder the transcoded image is appended to (non-NULL).
 * @param[in]     scr     Scratch buffers for decode / downscale / encode (non-NULL).
 * @param[in]     id_off  String-pool offset of the image href / manifest id.
 * @param[in]     raw_len Length of the encoded source bytes in @p scr->image_raw.
 * @return Image index on success, or @ref k_ra8_book_nil on any failure (decode,
 *         gray-scratch capacity, downscale, encode, or builder overflow).
 * @retval k_ra8_book_nil Decode / capacity / downscale / encode / append failed.
 * @pre @p ctx and @p scr are non-NULL (caller-validated).
 * @pre @p scr->image_raw holds @p raw_len readable encoded bytes.
 * @post The image arena is unbound on return regardless of outcome.
 * @post On success the builder gains one raster image descriptor at
 *       @p scr->pixel_format depth.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_transcode_image(ra8_rabook_ctx_t*                    ctx,
                                         const ra8_rabook_pipeline_scratch_t* scr,
                                         uint32_t                             id_off,
                                         size_t                               raw_len)
{
  int sw   = 0;
  int sh   = 0;
  int comp = 0;
  ra8_img_arena_bind(scr->img_arena);
  stbi_uc* pixels =
    stbi_load_from_memory(scr->image_raw, /* alloc-allow: stb backed by ra8_img_arena */
                          (int)raw_len,
                          &sw,
                          &sh,
                          &comp,
                          (int)k_stbi_grey);
  if (pixels == nullptr) {
    ra8_img_arena_unbind();
    ra8_log_error(s_tag, "stb_image decode failed");
    return k_ra8_book_nil;
  }

  /* Downscale is opt-in: max_image_edge == 0 (the default) preserves the
   * source resolution so zoomable content (manga pages) keeps every pixel. */
  uint16_t ow = (uint16_t)sw;
  uint16_t oh = (uint16_t)sh;
  if (scr->max_image_edge != 0U) {
    ra8_rabook_gray4_output_dims((uint16_t)sw, (uint16_t)sh, scr->max_image_edge, &ow, &oh);
  }

  const uint8_t* gray_src =
    internal_downscale_if_needed(scr, pixels, (uint16_t)sw, (uint16_t)sh, ow, oh);
  if (gray_src == nullptr) {
    stbi_image_free(pixels); /* alloc-allow: stb backed by ra8_img_arena */
    ra8_img_arena_unbind();
    return k_ra8_book_nil;
  }

  uint32_t  encoded_size = 0U;
  ra8_err_t enc_err      = internal_encode_gray(scr, gray_src, ow, oh, &encoded_size);
  stbi_image_free(pixels); /* alloc-allow: stb backed by ra8_img_arena */
  ra8_img_arena_unbind();

  if (enc_err != k_ra8_ok) {
    return k_ra8_book_nil;
  }

  return ra8_rabook_add_image(ctx,
                              id_off,
                              ow,
                              oh,
                              (uint8_t)k_ra8_book_image_gray4,
                              scr->pixel_format,
                              scr->image_raw,
                              encoded_size);
}

#endif /* RA8_RABOOK_NO_RASTER -- raster transcode helpers */

/**
 * @brief Find the TOC title for one spine chapter (first-entry-wins).
 * @details Scans the TOC entries in order; the first entry that maps to
 *          @p chapter_idx supplies the title. A miss (no TOC entry references the
 *          chapter, or the lookups fail) yields the empty string.
 * @param[in]  epub        Open book to read TOC entries from (non-NULL).
 * @param[in]  chapter_idx Spine index of the chapter whose title is wanted.
 * @param[in]  toc_count   Number of TOC entries to scan (0 disables the scan).
 * @param[out] entry_buf   Caller-owned scratch for the matched entry (non-NULL).
 * @return Pointer to the interned title string, or "" when no entry matches.
 * @retval "" No TOC entry references @p chapter_idx, or the readers failed.
 * @pre @p epub and @p entry_buf are non-NULL (caller-validated).
 * @pre @p toc_count does not exceed @ref k_pipeline_max_toc.
 * @post @p entry_buf holds the matched entry on a hit; indeterminate on a miss.
 * @post @p epub is not modified.
 * @note The returned pointer aliases @p entry_buf, so it is valid only until the
 *       next reuse of that buffer. Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static const char* internal_chapter_title(const ra8_epub_book_t* epub,
                                          uint16_t               chapter_idx,
                                          uint16_t               toc_count,
                                          ra8_epub_toc_entry_t*  entry_buf)
{
  for (uint16_t ti = 0U; ti < toc_count; ti++) {
    uint16_t ch_idx = 0U;
    if (ra8_epub_toc_entry_to_chapter(epub, ti, &ch_idx) == k_ra8_ok && ch_idx == chapter_idx) {
      if (ra8_epub_get_toc_entry(epub, ti, entry_buf) == k_ra8_ok) {
        return entry_buf->title;
      }
      break;
    }
  }
  return "";
}

/**
 * @brief Reject any NULL among the three arguments common to both compile entry
 *        points (@ref ra8_rabook_compile_from_epub and the buffer variant).
 * @details Centralises the shared null guards so each public function stays flat;
 *          the FS-write entry adds its own @p mount / @p out_path guards, the
 *          buffer entry its own @p out_blob / @p out_len guards.
 * @param[in] epub Open book pointer to validate.
 * @param[in] bufs Builder arenas pointer to validate.
 * @param[in] scr  Scratch buffers pointer to validate.
 * @return Error code.
 * @retval k_ra8_ok           All three arguments are non-NULL.
 * @retval k_ra8_err_null_ptr One of the arguments is NULL.
 * @pre The pointers, if non-NULL, address valid objects for the call.
 * @pre Called once at the top of a compile entry point.
 * @post No argument is modified (read-only validation).
 * @post On k_ra8_ok the three arguments are guaranteed non-NULL.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_check_compile_common(const ra8_epub_book_t*               epub,
                                               const ra8_rabook_buffers_t*          bufs,
                                               const ra8_rabook_pipeline_scratch_t* scr)
{
  RA8_CHECK_NULL_PTR(epub, s_tag, "epub");
  RA8_CHECK_NULL_PTR(bufs, s_tag, "bufs");
  RA8_CHECK_NULL_PTR(scr, s_tag, "scr");
  RA8_CHECK_NULL_PTR(scr->xml_workspace, s_tag, "scr->xml_workspace");
  return k_ra8_ok;
}

/**
 * @brief Emit each `text/css` manifest item as a stylesheet, in OPF order.
 * @details Walks the manifest in document order; for every item whose media-type
 *          is exactly `text/css`, loads its bytes via @ref ra8_epub_get_resource
 *          into @p scr->css, NUL-terminates them, interns the source string, and
 *          appends a book-wide stylesheet (scope @ref k_ra8_book_nil). An item
 *          absent from the archive (@ref k_ra8_err_not_found) is skipped, matching
 *          the desktop epub_compile.py "only if present" rule; the pass order and
 *          emit order match it too, so the stylesheet table + pool stay
 *          byte-identical.
 * @param[in,out] ctx  Builder the stylesheets are appended to (non-NULL).
 * @param[in]     scr  Scratch buffers (provides @p css / @p css_cap), non-NULL.
 * @param[in,out] epub Open book the CSS resources are read from (non-NULL).
 * @return Error code.
 * @retval k_ra8_ok        All present CSS items added (or none declared).
 * @retval <reader error> Any non-not_found error loading a CSS resource.
 * @pre @p ctx, @p scr and @p epub are non-NULL (caller-validated).
 * @pre @p scr->css has capacity for the largest stylesheet plus a NUL.
 * @post Every present `text/css` item is appended in OPF document order.
 * @post On error the builder may have latched its sticky-fail flag.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_compile_stylesheets(ra8_rabook_ctx_t*                    ctx,
                                              const ra8_rabook_pipeline_scratch_t* scr,
                                              ra8_epub_book_t*                     epub)
{
  const uint16_t count = ra8_epub_manifest_count(epub);
  for (uint16_t i = 0U; i < count; i++) {
    const ra8_epub_manifest_item_t* item = ra8_epub_manifest_item(epub, i);
    if (item == nullptr) {
      continue;
    }
    if (strcmp(item->media_type, "text/css") != 0) {
      continue;
    }
    size_t    got = 0U;
    ra8_err_t err =
      ra8_epub_get_resource(epub, item->href, (uint8_t*)scr->css, scr->css_cap - 1U, &got);
    if (err == k_ra8_err_not_found) {
      continue; /* desktop skips a css item absent from the archive */
    }
    if (err != k_ra8_ok) {
      return err;
    }
    scr->css[got]             = '\0';
    const uint32_t source_off = ra8_rabook_intern(ctx, scr->css);
    (void)ra8_rabook_add_stylesheet(ctx, source_off, (uint32_t)k_ra8_book_nil);
  }
  return k_ra8_ok;
}

/**
 * @brief Add one manifest item to the builder if it is an image.
 * @details Mirrors the desktop epub_compile.py image arm exactly: a
 *          @c image/svg+xml item is stored verbatim (vector, width=height=0); any
 *          other @c image/ * item is decoded + transcoded to 4-bpp gray via
 *          @ref internal_transcode_image (downscaled only above the panel edge). The item
 *          href is interned as the image id. A non-image item, an item absent from
 *          the archive, or one that fails to decode is skipped (returns nil) -- the
 *          desktop's @c try/except @c pass -- so one bad image never fails the
 *          whole compile.
 * @param[in,out] ctx  Builder the image is appended to (non-NULL).
 * @param[in]     scr  Scratch buffers for the resource load / transcode (non-NULL).
 * @param[in,out] epub Open book the resource bytes are read from (non-NULL).
 * @param[in]     item Manifest item to consider (non-NULL).
 * @return Image-table index of the added image, or @ref k_ra8_book_nil when the
 *         item is not an image, is absent, or failed to decode.
 * @retval k_ra8_book_nil Not an image / absent / undecodable (skipped).
 * @pre @p ctx, @p scr, @p epub and @p item are non-NULL (caller-validated).
 * @pre @p scr->image_raw / @p image_cap can hold the resource bytes.
 * @post On a non-nil return the builder gained one image descriptor.
 * @post @p scr->image_raw is clobbered (resource bytes, then encoded nibbles).
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static uint32_t internal_add_manifest_image(ra8_rabook_ctx_t*                    ctx,
                                            const ra8_rabook_pipeline_scratch_t* scr,
                                            ra8_epub_book_t*                     epub,
                                            const ra8_epub_manifest_item_t*      item)
{
  const char* const img_prefix = "image/";
  uint8_t           fmt        = (uint8_t)k_ra8_book_image_gray4;
  if (strcmp(item->media_type, "image/svg+xml") == 0) {
    fmt = (uint8_t)k_ra8_book_image_svg;
  } else if (strncmp(item->media_type, img_prefix, strlen(img_prefix)) != 0) {
    return (uint32_t)k_ra8_book_nil; /* not an image item */
  }

#ifdef RA8_RABOOK_NO_RASTER
  if (fmt != (uint8_t)k_ra8_book_image_svg) {
    ra8_log_error(s_tag, "raster image skipped (text/CSS/SVG-only build)");
    return (uint32_t)k_ra8_book_nil;
  }
#endif

  size_t    got = 0U;
  ra8_err_t err = ra8_epub_get_resource(epub, item->href, scr->image_raw, scr->image_cap, &got);
  if (err != k_ra8_ok) {
    return (uint32_t)k_ra8_book_nil; /* absent / unreadable -> skip, like the desktop */
  }

  const uint32_t id_off = ra8_rabook_intern(ctx, item->href);
  if (fmt == (uint8_t)k_ra8_book_image_svg) {
    return ra8_rabook_add_image(ctx,
                                id_off,
                                0U,
                                0U,
                                (uint8_t)k_ra8_book_image_svg,
                                (uint8_t)k_ra8_book_pixfmt_gray4, /* unused for SVG; store 0 */
                                scr->image_raw,
                                (uint32_t)got);
  }
#ifndef RA8_RABOOK_NO_RASTER
  return internal_transcode_image(ctx, scr, id_off, got);
#else
  return (uint32_t)k_ra8_book_nil; /* unreachable: raster returned nil above */
#endif
}

/**
 * @brief Compile every manifest image into the builder, resolving the cover.
 * @details Walks the manifest in OPF document order -- the same order the desktop
 *          epub_compile.py iterates @c manifest.items() -- adding each image item
 *          via @ref internal_add_manifest_image so the image table indices match. The
 *          cover index is the table index of the item whose href equals
 *          @c epub->cover_path (ra8_epub already resolves that from
 *          `properties="cover-image"` or the legacy `<meta name="cover">`), which
 *          is the same image the desktop's @c id_to_image[cover_id] yields.
 *
 *          When @p scr->skip_images is set the whole stage is a no-op: no image
 *          is added and the cover index stays nil, exactly like the desktop
 *          @c --no-images path (its @c manifest.items() image loop iterates the
 *          empty tuple and @c cover_id never resolves), producing a text/CSS-only
 *          blob byte-identical to the desktop @c --no-images golden.
 * @param[in,out] ctx             Builder the images are appended to (non-NULL).
 * @param[in]     scr             Scratch buffers for load / transcode (non-NULL).
 * @param[in,out] epub            Open book providing the manifest (non-NULL).
 * @param[out]    cover_index_out Receives the cover image index, or nil (non-NULL).
 * @return Error code.
 * @retval k_ra8_ok Images walked; @p *cover_index_out set (nil if no cover image).
 * @pre @p ctx, @p scr, @p epub and @p cover_index_out are non-NULL.
 * @pre The manifest was populated by @ref ra8_epub_open.
 * @post Every decodable image item has an image-table entry, in OPF order, OR
 *       (when @p scr->skip_images) the image table is left empty.
 * @post @p *cover_index_out is the cover image index, or nil when absent / skipped.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_compile_images(ra8_rabook_ctx_t*                    ctx,
                                         const ra8_rabook_pipeline_scratch_t* scr,
                                         ra8_epub_book_t*                     epub,
                                         uint32_t*                            cover_index_out)
{
  *cover_index_out = (uint32_t)k_ra8_book_nil;
  if (scr->skip_images) {
    return k_ra8_ok; /* desktop --no-images: empty image loop, cover stays nil */
  }
  const uint16_t count = ra8_epub_manifest_count(epub);
  for (uint16_t i = 0U; i < count; i++) {
    const ra8_epub_manifest_item_t* item = ra8_epub_manifest_item(epub, i);
    if (item == nullptr) {
      continue;
    }
    const uint32_t idx = internal_add_manifest_image(ctx, scr, epub, item);
    if (idx == (uint32_t)k_ra8_book_nil) {
      continue;
    }
    if (strcmp(item->href, epub->cover_path) == 0) {
      *cover_index_out = idx;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Parse every spine chapter into the builder DOM.
 * @details Loads each chapter's XHTML into @p scr->xhtml, resolves its TOC title,
 *          and hands it to @ref ra8_rabook_xml_parse_chapter, stopping at the first
 *          error.
 * @param[in,out] epub Open book providing the spine chapters (non-NULL).
 * @param[in]     scr  Scratch buffers for the chapter XHTML load (non-NULL).
 * @param[in,out] ctx  Builder the chapter DOMs are appended to (non-NULL).
 * @return Error code.
 * @retval k_ra8_ok        Every spine chapter parsed and added.
 * @retval <reader error> The first chapter count / load failure, propagated.
 * @retval <parser error> The first @ref ra8_rabook_xml_parse_chapter failure.
 * @pre @p epub, @p scr and @p ctx are non-NULL (caller-validated).
 * @pre @p scr->xhtml is large enough for the largest chapter.
 * @post On k_ra8_ok every spine chapter has a chapter-table entry.
 * @post On error parsing stops; the builder holds the chapters processed so far.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_compile_chapters(ra8_epub_book_t*                     epub,
                                           const ra8_rabook_pipeline_scratch_t* scr,
                                           ra8_rabook_ctx_t*                    ctx)
{
  uint16_t  chapter_count = 0U;
  ra8_err_t err           = ra8_epub_get_chapter_count(epub, &chapter_count);
  if (err != k_ra8_ok) {
    return err;
  }

  uint16_t toc_count = 0U;
  (void)ra8_epub_get_toc_count(epub, &toc_count);

  ra8_epub_toc_entry_t toc_entry = {};

  for (uint16_t ci = 0U; ci < chapter_count; ci++) {
    size_t got_len = 0U;
    err            = ra8_epub_load_chapter(epub, ci, scr->xhtml, scr->xhtml_cap, &got_len);
    if (err != k_ra8_ok) {
      return err;
    }

    const char* ch_title = internal_chapter_title(epub, ci, toc_count, &toc_entry);
    const char* ch_href  = epub->chapter_paths[ci];

    err =
      ra8_rabook_xml_parse_chapter(scr->xhtml, got_len, ctx, ch_href, ch_title, scr->xml_workspace);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Intern the Dublin Core metadata and record it (the final emit stage).
 * @details Runs LAST, after the chapters, so the title/author/language/identifier
 *          strings land after the chapter DOM strings in the pool -- matching the
 *          desktop epub_compile.py serialize(meta) order the #151 byte-identity
 *          gate requires. Interning metadata earlier shifts every string offset.
 * @param[in,out] ctx               Builder receiving the metadata (non-NULL).
 * @param[in]     epub              Open book to read the Dublin Core fields from.
 * @param[in]     cover_image_index Cover image index resolved earlier, or nil.
 * @return Error code.
 * @retval k_ra8_ok Metadata interned and recorded in the builder header.
 * @retval k_ra8_err_invalid_arg @ref ra8_epub_get_metadata rejected @p epub.
 * @retval k_ra8_err_no_mem The string pool or header could not hold the metadata.
 * @pre @p ctx is initialised and the chapters are already emitted.
 * @pre @p epub is an open book record.
 * @post On success the builder header carries the four metadata offsets.
 * @post The pool order matches the desktop emit (metadata strings last).
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t
internal_compile_metadata(ra8_rabook_ctx_t* ctx, ra8_epub_book_t* epub, uint32_t cover_image_index)
{
  ra8_epub_metadata_t meta = {};
  ra8_err_t           err  = ra8_epub_get_metadata(epub, &meta);
  if (err != k_ra8_ok) {
    return err;
  }
  const uint32_t title_off      = ra8_rabook_intern(ctx, meta.title);
  const uint32_t author_off     = ra8_rabook_intern(ctx, meta.author);
  const uint32_t language_off   = ra8_rabook_intern(ctx, meta.language);
  const uint32_t identifier_off = ra8_rabook_intern(ctx, meta.identifier);
  return ra8_rabook_set_metadata(ctx,
                                 title_off,
                                 author_off,
                                 language_off,
                                 identifier_off,
                                 cover_image_index);
}

/**
 * @brief Run the compile stages and finalize the RABOOK1 blob in @p bufs->out.
 * @details The shared core of both public entry points: initialises the builder
 *          and emits the stages in the desktop epub_compile.py order
 *          (stylesheets -> images -> chapters -> metadata interned LAST) so the
 *          blob is byte-identical, then finalises. The blob lives in @p bufs->out
 *          and stays valid as long as that arena does.
 * @param[in]  epub     Open book (validated non-NULL by the caller).
 * @param[in]  bufs     Builder arenas (validated non-NULL by the caller).
 * @param[in]  scr      Scratch buffers (validated non-NULL by the caller).
 * @param[out] out_blob Receives a pointer to the finalized blob in @p bufs->out.
 * @param[out] out_len  Receives the blob length in bytes.
 * @return Error code.
 * @retval k_ra8_ok     Blob emitted; @p *out_blob / @p *out_len set.
 * @retval <stage err> The first failing stage / finalize error, propagated.
 * @pre @p epub, @p bufs, @p scr, @p out_blob and @p out_len are non-NULL.
 * @pre @p epub->in_use == 1 and the arenas are sized for the book.
 * @post On k_ra8_ok @p *out_blob addresses a valid RABOOK1 blob of @p *out_len.
 * @post On error @p bufs->out may hold a partial layout.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_compile_to_blob(ra8_epub_book_t*                     epub,
                                          const ra8_rabook_buffers_t*          bufs,
                                          const ra8_rabook_pipeline_scratch_t* scr,
                                          const void**                         out_blob,
                                          uint32_t*                            out_len)
{
  ra8_rabook_ctx_t ctx = {};
  ra8_err_t        err = ra8_rabook_compile_init(&ctx, bufs);
  if (err != k_ra8_ok) {
    return err;
  }
  /* Emit in the desktop epub_compile.py order so the blob is byte-identical:
   * stylesheets, images (cover resolved within), chapters, metadata interned LAST. */
  err = internal_compile_stylesheets(&ctx, scr, epub);
  if (err != k_ra8_ok) {
    return err;
  }
  uint32_t cover_image_index = (uint32_t)k_ra8_book_nil;
  err                        = internal_compile_images(&ctx, scr, epub, &cover_image_index);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_compile_chapters(epub, scr, &ctx);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_compile_metadata(&ctx, epub, cover_image_index);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_rabook_finalize(&ctx, out_blob, out_len);
}

/* -------------------------------------------------------------------------- */
/* Public API */
/* -------------------------------------------------------------------------- */

ra8_err_t ra8_rabook_compile_from_epub_to_buffer(ra8_epub_book_t*                     epub,
                                                 const ra8_rabook_buffers_t*          bufs,
                                                 const ra8_rabook_pipeline_scratch_t* scr,
                                                 const void**                         out_blob,
                                                 uint32_t*                            out_len)
{
  ra8_err_t err = internal_check_compile_common(epub, bufs, scr);
  if (err != k_ra8_ok) {
    return err;
  }
  RA8_CHECK_NULL_PTR(out_blob, s_tag, "out_blob");
  RA8_CHECK_NULL_PTR(out_len, s_tag, "out_len");
  return internal_compile_to_blob(epub, bufs, scr, out_blob, out_len);
}

ra8_err_t ra8_rabook_compile_from_epub(ra8_epub_book_t*                     epub,
                                       const ra8_rabook_buffers_t*          bufs,
                                       const ra8_rabook_pipeline_scratch_t* scr,
                                       ra8_fs_mount_t*                      mount,
                                       const char*                          out_path)
{
  ra8_err_t err = internal_check_compile_common(epub, bufs, scr);
  if (err != k_ra8_ok) {
    return err;
  }
  RA8_CHECK_NULL_PTR(mount, s_tag, "mount");
  RA8_CHECK_NULL_PTR(out_path, s_tag, "out_path");

  const void* blob     = nullptr;
  uint32_t    blob_len = 0U;
  err                  = internal_compile_to_blob(epub, bufs, scr, &blob, &blob_len);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_fs_write_file(mount, out_path, (const uint8_t*)blob, blob_len);
}
