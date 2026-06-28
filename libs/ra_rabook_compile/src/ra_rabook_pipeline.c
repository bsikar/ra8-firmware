/**
 * @file ra_rabook_pipeline.c
 * @brief End-to-end EPUB -> RABOOK1 compile pipeline (#149).
 * @since Version 0.1.0
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_rabook_pipeline.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra_attributes.h"
#include "ra_book.h"
#include "ra_check.h"
#include "ra_epub.h"
#include "ra_err.h"
#include "ra_fs.h"
#include "ra_img_arena.h"
#include "ra_log.h"
#include "ra_rabook_compile.h"
#include "ra_rabook_gray4.h"
#include "ra_rabook_xml_shim.h"
#include "ra_reflow_image.h"
#include "stb_image.h"

/* -------------------------------------------------------------------------- */
/* Private constants */
/* -------------------------------------------------------------------------- */

/**
 * @enum ra_pipeline_limits_t
 * @brief Static bounds used for NASA Rule 2 loop annotations.
 * @since Version 0.1.0
 */
typedef enum : uint16_t {
  k_pipeline_max_chapters = 64U, /**< == k_ra_epub_max_chapters (cross-check). */
  k_pipeline_max_toc      = 64U, /**< == k_ra_epub_max_toc (cross-check).      */
} ra_pipeline_limits_t;

static_assert((uint16_t)k_pipeline_max_chapters == (uint16_t)k_ra_epub_max_chapters,
              "pipeline chapter cap must match epub cap");
static_assert((uint16_t)k_pipeline_max_toc == (uint16_t)k_ra_epub_max_toc,
              "pipeline toc cap must match epub cap");

/**
 * @enum ra_pipeline_stbi_t
 * @brief stb_image channel-count selector (no magic literals).
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_stbi_grey = 1U, /**< Request 8-bit grayscale output from stb_image. */
} ra_pipeline_stbi_t;

static const char* const s_tag = "ra_rabook_pipeline";

/* -------------------------------------------------------------------------- */
/* Private helpers */
/* -------------------------------------------------------------------------- */

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
RA_INTERNAL
static const uint8_t* s_downscale_if_needed(const ra_rabook_pipeline_scratch_t* scr,
                                            const uint8_t*                      pixels,
                                            uint16_t                            sw,
                                            uint16_t                            sh,
                                            uint16_t                            ow,
                                            uint16_t                            oh)
{
  if (ow == sw && oh == sh) {
    return pixels;
  }
  if ((uint32_t)ow * oh > scr->gray_cap) {
    ra_log_error(s_tag, "gray scratch too small for downscale");
    return nullptr;
  }
  if (ra_rabook_gray4_downscale(pixels, sw, sh, scr->gray, ow, oh) != k_ra_ok) {
    return nullptr;
  }
  return scr->gray;
}

/**
 * @brief Decode one raw image, transcode to 4-bpp gray, add to builder.
 * @details Binds @p scr->img_arena before calling stb_image and unbinds on every
 *          return path.  The encoded nibbles are written into @p scr->image_raw
 *          (reusing the buffer that held the raw bytes), so the source and encode
 *          buffers must not overlap (see @ref ra_rabook_pipeline_scratch_t).
 * @param[in,out] ctx     Builder the transcoded image is appended to (non-NULL).
 * @param[in]     scr     Scratch buffers for decode / downscale / encode (non-NULL).
 * @param[in]     id_off  String-pool offset of the image href / manifest id.
 * @param[in]     raw_len Length of the encoded source bytes in @p scr->image_raw.
 * @return Image index on success, or @ref k_ra_book_nil on any failure (decode,
 *         gray-scratch capacity, downscale, encode, or builder overflow).
 * @retval k_ra_book_nil Decode / capacity / downscale / encode / append failed.
 * @pre @p ctx and @p scr are non-NULL (caller-validated).
 * @pre @p scr->image_raw holds @p raw_len readable encoded bytes.
 * @post The image arena is unbound on return regardless of outcome.
 * @post On success the builder gains one gray4 image descriptor.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA_INTERNAL
static uint32_t s_transcode_image(ra_rabook_ctx_t*                    ctx,
                                  const ra_rabook_pipeline_scratch_t* scr,
                                  uint32_t                            id_off,
                                  size_t                              raw_len)
{
  int sw   = 0;
  int sh   = 0;
  int comp = 0;
  ra_img_arena_bind(scr->img_arena);
  stbi_uc* pixels =
    stbi_load_from_memory(scr->image_raw, /* alloc-allow: stb backed by ra_img_arena */
                          (int)raw_len,
                          &sw,
                          &sh,
                          &comp,
                          (int)k_stbi_grey);
  if (pixels == nullptr) {
    ra_img_arena_unbind();
    ra_log_error(s_tag, "stb_image decode failed");
    return k_ra_book_nil;
  }

  uint16_t ow = 0U;
  uint16_t oh = 0U;
  ra_rabook_gray4_output_dims((uint16_t)sw, (uint16_t)sh, k_ra_rabook_gray4_max_edge, &ow, &oh);

  const uint8_t* gray_src = s_downscale_if_needed(scr, pixels, (uint16_t)sw, (uint16_t)sh, ow, oh);
  if (gray_src == nullptr) {
    stbi_image_free(pixels); /* alloc-allow: stb backed by ra_img_arena */
    ra_img_arena_unbind();
    return k_ra_book_nil;
  }

  uint32_t encoded_size = 0U;
  ra_err_t enc_err      = ra_rabook_gray4_encode(gray_src,
                                                 ow,
                                                 oh,
                                                 scr->image_raw,
                                                 (uint32_t)scr->image_cap,
                                                 &encoded_size);
  stbi_image_free(pixels); /* alloc-allow: stb backed by ra_img_arena */
  ra_img_arena_unbind();

  if (enc_err != k_ra_ok) {
    return k_ra_book_nil;
  }

  return ra_rabook_add_image(ctx,
                             id_off,
                             ow,
                             oh,
                             (uint8_t)k_ra_book_image_gray4,
                             scr->image_raw,
                             encoded_size);
}

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
RA_INTERNAL
static const char* s_chapter_title(const ra_epub_book_t* epub,
                                   uint16_t              chapter_idx,
                                   uint16_t              toc_count,
                                   ra_epub_toc_entry_t*  entry_buf)
{
  for (uint16_t ti = 0U; ti < toc_count; ti++) {
    uint16_t ch_idx = 0U;
    if (ra_epub_toc_entry_to_chapter(epub, ti, &ch_idx) == k_ra_ok && ch_idx == chapter_idx) {
      if (ra_epub_get_toc_entry(epub, ti, entry_buf) == k_ra_ok) {
        return entry_buf->title;
      }
      break;
    }
  }
  return "";
}

/**
 * @brief Reject any NULL argument to @ref ra_rabook_compile_from_epub.
 * @details Centralises the five entry-point null guards so the public function
 *          stays flat. Each guard names its argument in the log line.
 * @param[in] epub     Open book pointer to validate.
 * @param[in] bufs     Builder arenas pointer to validate.
 * @param[in] scr      Scratch buffers pointer to validate.
 * @param[in] mount    Filesystem mount pointer to validate.
 * @param[in] out_path Output path pointer to validate.
 * @return Error code.
 * @retval k_ra_ok           Every argument is non-NULL.
 * @retval k_ra_err_null_ptr One of the arguments is NULL.
 * @pre The pointers, if non-NULL, address valid objects for the call.
 * @pre Called once at the top of the compile entry point.
 * @post No argument is modified (read-only validation).
 * @post On k_ra_ok all five arguments are guaranteed non-NULL.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA_INTERNAL
static ra_err_t s_check_compile_args(const ra_epub_book_t*               epub,
                                     const ra_rabook_buffers_t*          bufs,
                                     const ra_rabook_pipeline_scratch_t* scr,
                                     const ra_fs_mount_t*                mount,
                                     const char*                         out_path)
{
  RA_CHECK_NULL_PTR(epub, s_tag, "epub");
  RA_CHECK_NULL_PTR(bufs, s_tag, "bufs");
  RA_CHECK_NULL_PTR(scr, s_tag, "scr");
  RA_CHECK_NULL_PTR(mount, s_tag, "mount");
  RA_CHECK_NULL_PTR(out_path, s_tag, "out_path");
  return k_ra_ok;
}

/**
 * @brief Emit each `text/css` manifest item as a stylesheet, in OPF order.
 * @details Walks the manifest in document order; for every item whose media-type
 *          is exactly `text/css`, loads its bytes via @ref ra_epub_get_resource
 *          into @p scr->css, NUL-terminates them, interns the source string, and
 *          appends a book-wide stylesheet (scope @ref k_ra_book_nil). An item
 *          absent from the archive (@ref k_ra_err_not_found) is skipped, matching
 *          the desktop epub_compile.py "only if present" rule; the pass order and
 *          emit order match it too, so the stylesheet table + pool stay
 *          byte-identical.
 * @param[in,out] ctx  Builder the stylesheets are appended to (non-NULL).
 * @param[in]     scr  Scratch buffers (provides @p css / @p css_cap), non-NULL.
 * @param[in,out] epub Open book the CSS resources are read from (non-NULL).
 * @return Error code.
 * @retval k_ra_ok        All present CSS items added (or none declared).
 * @retval <reader error> Any non-not_found error loading a CSS resource.
 * @pre @p ctx, @p scr and @p epub are non-NULL (caller-validated).
 * @pre @p scr->css has capacity for the largest stylesheet plus a NUL.
 * @post Every present `text/css` item is appended in OPF document order.
 * @post On error the builder may have latched its sticky-fail flag.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA_INTERNAL
static ra_err_t s_compile_stylesheets(ra_rabook_ctx_t*                    ctx,
                                      const ra_rabook_pipeline_scratch_t* scr,
                                      ra_epub_book_t*                     epub)
{
  const uint16_t count = ra_epub_manifest_count(epub);
  for (uint16_t i = 0U; i < count; i++) {
    const ra_epub_manifest_item_t* item = ra_epub_manifest_item(epub, i);
    if (item == nullptr) {
      continue;
    }
    if (strcmp(item->media_type, "text/css") != 0) {
      continue;
    }
    size_t   got = 0U;
    ra_err_t err =
      ra_epub_get_resource(epub, item->href, (uint8_t*)scr->css, scr->css_cap - 1U, &got);
    if (err == k_ra_err_not_found) {
      continue; /* desktop skips a css item absent from the archive */
    }
    if (err != k_ra_ok) {
      return err;
    }
    scr->css[got]             = '\0';
    const uint32_t source_off = ra_rabook_intern(ctx, scr->css);
    (void)ra_rabook_add_stylesheet(ctx, source_off, (uint32_t)k_ra_book_nil);
  }
  return k_ra_ok;
}

/**
 * @brief Add one manifest item to the builder if it is an image.
 * @details Mirrors the desktop epub_compile.py image arm exactly: a
 *          @c image/svg+xml item is stored verbatim (vector, width=height=0); any
 *          other @c image/ * item is decoded + transcoded to 4-bpp gray via
 *          @ref s_transcode_image (downscaled only above the panel edge). The item
 *          href is interned as the image id. A non-image item, an item absent from
 *          the archive, or one that fails to decode is skipped (returns nil) -- the
 *          desktop's @c try/except @c pass -- so one bad image never fails the
 *          whole compile.
 * @param[in,out] ctx  Builder the image is appended to (non-NULL).
 * @param[in]     scr  Scratch buffers for the resource load / transcode (non-NULL).
 * @param[in,out] epub Open book the resource bytes are read from (non-NULL).
 * @param[in]     item Manifest item to consider (non-NULL).
 * @return Image-table index of the added image, or @ref k_ra_book_nil when the
 *         item is not an image, is absent, or failed to decode.
 * @retval k_ra_book_nil Not an image / absent / undecodable (skipped).
 * @pre @p ctx, @p scr, @p epub and @p item are non-NULL (caller-validated).
 * @pre @p scr->image_raw / @p image_cap can hold the resource bytes.
 * @post On a non-nil return the builder gained one image descriptor.
 * @post @p scr->image_raw is clobbered (resource bytes, then encoded nibbles).
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA_INTERNAL
static uint32_t s_add_manifest_image(ra_rabook_ctx_t*                    ctx,
                                     const ra_rabook_pipeline_scratch_t* scr,
                                     ra_epub_book_t*                     epub,
                                     const ra_epub_manifest_item_t*      item)
{
  const char* const img_prefix = "image/";
  uint8_t           fmt        = (uint8_t)k_ra_book_image_gray4;
  if (strcmp(item->media_type, "image/svg+xml") == 0) {
    fmt = (uint8_t)k_ra_book_image_svg;
  } else if (strncmp(item->media_type, img_prefix, strlen(img_prefix)) != 0) {
    return (uint32_t)k_ra_book_nil; /* not an image item */
  }

  size_t   got = 0U;
  ra_err_t err = ra_epub_get_resource(epub, item->href, scr->image_raw, scr->image_cap, &got);
  if (err != k_ra_ok) {
    return (uint32_t)k_ra_book_nil; /* absent / unreadable -> skip, like the desktop */
  }

  const uint32_t id_off = ra_rabook_intern(ctx, item->href);
  if (fmt == (uint8_t)k_ra_book_image_svg) {
    return ra_rabook_add_image(ctx,
                               id_off,
                               0U,
                               0U,
                               (uint8_t)k_ra_book_image_svg,
                               scr->image_raw,
                               (uint32_t)got);
  }
  return s_transcode_image(ctx, scr, id_off, got);
}

/**
 * @brief Compile every manifest image into the builder, resolving the cover.
 * @details Walks the manifest in OPF document order -- the same order the desktop
 *          epub_compile.py iterates @c manifest.items() -- adding each image item
 *          via @ref s_add_manifest_image so the image table indices match. The
 *          cover index is the table index of the item whose href equals
 *          @c epub->cover_path (ra_epub already resolves that from
 *          @c properties="cover-image" or the legacy @c <meta name="cover">), which
 *          is the same image the desktop's @c id_to_image[cover_id] yields.
 * @param[in,out] ctx             Builder the images are appended to (non-NULL).
 * @param[in]     scr             Scratch buffers for load / transcode (non-NULL).
 * @param[in,out] epub            Open book providing the manifest (non-NULL).
 * @param[out]    cover_index_out Receives the cover image index, or nil (non-NULL).
 * @return Error code.
 * @retval k_ra_ok Images walked; @p *cover_index_out set (nil if no cover image).
 * @pre @p ctx, @p scr, @p epub and @p cover_index_out are non-NULL.
 * @pre The manifest was populated by @ref ra_epub_open.
 * @post Every decodable image item has an image-table entry, in OPF order.
 * @post @p *cover_index_out is the cover image index, or nil when absent.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA_INTERNAL
static ra_err_t s_compile_images(ra_rabook_ctx_t*                    ctx,
                                 const ra_rabook_pipeline_scratch_t* scr,
                                 ra_epub_book_t*                     epub,
                                 uint32_t*                           cover_index_out)
{
  *cover_index_out     = (uint32_t)k_ra_book_nil;
  const uint16_t count = ra_epub_manifest_count(epub);
  for (uint16_t i = 0U; i < count; i++) {
    const ra_epub_manifest_item_t* item = ra_epub_manifest_item(epub, i);
    if (item == nullptr) {
      continue;
    }
    const uint32_t idx = s_add_manifest_image(ctx, scr, epub, item);
    if (idx == (uint32_t)k_ra_book_nil) {
      continue;
    }
    if (strcmp(item->href, epub->cover_path) == 0) {
      *cover_index_out = idx;
    }
  }
  return k_ra_ok;
}

/**
 * @brief Parse every spine chapter into the builder DOM.
 * @details Loads each chapter's XHTML into @p scr->xhtml, resolves its TOC title,
 *          and hands it to @ref ra_rabook_xml_parse_chapter, stopping at the first
 *          error.
 * @param[in,out] epub Open book providing the spine chapters (non-NULL).
 * @param[in]     scr  Scratch buffers for the chapter XHTML load (non-NULL).
 * @param[in,out] ctx  Builder the chapter DOMs are appended to (non-NULL).
 * @return Error code.
 * @retval k_ra_ok        Every spine chapter parsed and added.
 * @retval <reader error> The first chapter count / load failure, propagated.
 * @retval <parser error> The first @ref ra_rabook_xml_parse_chapter failure.
 * @pre @p epub, @p scr and @p ctx are non-NULL (caller-validated).
 * @pre @p scr->xhtml is large enough for the largest chapter.
 * @post On k_ra_ok every spine chapter has a chapter-table entry.
 * @post On error parsing stops; the builder holds the chapters processed so far.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA_INTERNAL
static ra_err_t s_compile_chapters(ra_epub_book_t*                     epub,
                                   const ra_rabook_pipeline_scratch_t* scr,
                                   ra_rabook_ctx_t*                    ctx)
{
  uint16_t chapter_count = 0U;
  ra_err_t err           = ra_epub_get_chapter_count(epub, &chapter_count);
  if (err != k_ra_ok) {
    return err;
  }

  uint16_t toc_count = 0U;
  (void)ra_epub_get_toc_count(epub, &toc_count);

  ra_epub_toc_entry_t toc_entry = {};

  for (uint16_t ci = 0U; ci < chapter_count; ci++) {
    size_t got_len = 0U;
    err            = ra_epub_load_chapter(epub, ci, scr->xhtml, scr->xhtml_cap, &got_len);
    if (err != k_ra_ok) {
      return err;
    }

    const char* ch_title = s_chapter_title(epub, ci, toc_count, &toc_entry);
    const char* ch_href  = epub->chapter_paths[ci];

    err = ra_rabook_xml_parse_chapter(scr->xhtml, got_len, ctx, ch_href, ch_title);
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
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
 * @retval k_ra_ok Metadata interned and recorded in the builder header.
 * @retval k_ra_err_invalid_arg @ref ra_epub_get_metadata rejected @p epub.
 * @retval k_ra_err_no_mem The string pool or header could not hold the metadata.
 * @pre @p ctx is initialised and the chapters are already emitted.
 * @pre @p epub is an open book record.
 * @post On success the builder header carries the four metadata offsets.
 * @post The pool order matches the desktop emit (metadata strings last).
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA_INTERNAL
static ra_err_t
s_compile_metadata(ra_rabook_ctx_t* ctx, ra_epub_book_t* epub, uint32_t cover_image_index)
{
  ra_epub_metadata_t meta = {};
  ra_err_t           err  = ra_epub_get_metadata(epub, &meta);
  if (err != k_ra_ok) {
    return err;
  }
  const uint32_t title_off      = ra_rabook_intern(ctx, meta.title);
  const uint32_t author_off     = ra_rabook_intern(ctx, meta.author);
  const uint32_t language_off   = ra_rabook_intern(ctx, meta.language);
  const uint32_t identifier_off = ra_rabook_intern(ctx, meta.identifier);
  return ra_rabook_set_metadata(ctx,
                                title_off,
                                author_off,
                                language_off,
                                identifier_off,
                                cover_image_index);
}

/* -------------------------------------------------------------------------- */
/* Public API */
/* -------------------------------------------------------------------------- */

ra_err_t ra_rabook_compile_from_epub(ra_epub_book_t*                     epub,
                                     const ra_rabook_buffers_t*          bufs,
                                     const ra_rabook_pipeline_scratch_t* scr,
                                     ra_fs_mount_t*                      mount,
                                     const char*                         out_path)
{
  ra_err_t err = s_check_compile_args(epub, bufs, scr, mount, out_path);
  if (err != k_ra_ok) {
    return err;
  }

  ra_rabook_ctx_t ctx = {};
  err                 = ra_rabook_compile_init(&ctx, bufs);
  if (err != k_ra_ok) {
    return err;
  }

  /* Emit in the desktop epub_compile.py order so the blob is byte-identical:
   * stylesheets, images (cover resolved within), chapters, metadata interned LAST. */
  err = s_compile_stylesheets(&ctx, scr, epub);
  if (err != k_ra_ok) {
    return err;
  }
  uint32_t cover_image_index = (uint32_t)k_ra_book_nil;
  err                        = s_compile_images(&ctx, scr, epub, &cover_image_index);
  if (err != k_ra_ok) {
    return err;
  }
  err = s_compile_chapters(epub, scr, &ctx);
  if (err != k_ra_ok) {
    return err;
  }
  err = s_compile_metadata(&ctx, epub, cover_image_index);
  if (err != k_ra_ok) {
    return err;
  }

  /* 4. Finalize blob. */
  const void* blob     = nullptr;
  uint32_t    blob_len = 0U;
  err                  = ra_rabook_finalize(&ctx, &blob, &blob_len);
  if (err != k_ra_ok) {
    return err;
  }

  /* 6. Write to filesystem. */
  return ra_fs_write_file(mount, out_path, (const uint8_t*)blob, blob_len);
}
