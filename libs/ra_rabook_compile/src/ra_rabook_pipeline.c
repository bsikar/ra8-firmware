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
    stbi_load_from_memory(scr->image_raw,
                          (int)raw_len,
                          &sw,
                          &sh,
                          &comp,
                          (int)k_stbi_grey); /* alloc-allow: stb backed by ra_img_arena */
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
    stbi_image_free(pixels);
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
  stbi_image_free(pixels);
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
  RA_BOUNDED_LOOP(k_pipeline_max_toc)
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
 * @brief Compile the cover image into the builder, if the EPUB has one.
 * @details Reads the cover bytes; a @ref k_ra_err_not_found result means the book
 *          legitimately has no cover, so the cover index stays nil and the call
 *          succeeds. A present cover that fails to transcode (decode, gray-scratch
 *          capacity, downscale, encode, or builder overflow) is surfaced as
 *          @ref k_ra_err_no_mem rather than silently dropped.
 * @param[in,out] ctx             Builder the cover image is appended to (non-NULL).
 * @param[in]     scr             Scratch buffers for decode / transcode (non-NULL).
 * @param[in,out] epub            Open book the cover is read from (non-NULL).
 * @param[out]    cover_index_out Receives the cover image index, or nil (non-NULL).
 * @return Error code.
 * @retval k_ra_ok            Cover added, or the book has no cover (index nil).
 * @retval k_ra_err_no_mem    A present cover could not be transcoded / stored.
 * @retval <reader error>     Any non-not_found error from the cover reader.
 * @pre @p ctx, @p scr, @p epub and @p cover_index_out are non-NULL.
 * @pre @p scr->image_raw is large enough for the encoded cover bytes.
 * @post @p *cover_index_out is set (nil when absent, a valid index on success).
 * @post On error @p *cover_index_out is nil and the builder may have latched fail.
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
RA_INTERNAL
static ra_err_t s_compile_cover(ra_rabook_ctx_t*                    ctx,
                                const ra_rabook_pipeline_scratch_t* scr,
                                ra_epub_book_t*                     epub,
                                uint32_t*                           cover_index_out)
{
  *cover_index_out = (uint32_t)k_ra_book_nil;

  size_t   raw_len = 0U;
  ra_err_t err     = ra_epub_get_cover_image(epub, scr->image_raw, scr->image_cap, &raw_len);
  if (err == k_ra_err_not_found) {
    return k_ra_ok; /* no cover -> nil index, success */
  }
  if (err != k_ra_ok) {
    return err;
  }

  const uint32_t cover_id_off = ra_rabook_intern(ctx, epub->cover_path);
  const uint32_t cover_index  = s_transcode_image(ctx, scr, cover_id_off, raw_len);
  if (cover_index == (uint32_t)k_ra_book_nil) {
    ra_log_error(s_tag, "cover present but failed to transcode");
    return k_ra_err_no_mem;
  }

  *cover_index_out = cover_index;
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

  RA_BOUNDED_LOOP(k_pipeline_max_chapters)
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

  /* 1. Metadata strings (interned before the cover so pool order is stable). */
  ra_epub_metadata_t meta = {};
  err                     = ra_epub_get_metadata(epub, &meta);
  if (err != k_ra_ok) {
    return err;
  }
  const uint32_t title_off      = ra_rabook_intern(&ctx, meta.title);
  const uint32_t author_off     = ra_rabook_intern(&ctx, meta.author);
  const uint32_t language_off   = ra_rabook_intern(&ctx, meta.language);
  const uint32_t identifier_off = ra_rabook_intern(&ctx, meta.identifier);

  /* 2. Cover image (absent is fine; a present-but-unencodable cover errors). */
  uint32_t cover_image_index = (uint32_t)k_ra_book_nil;
  err                        = s_compile_cover(&ctx, scr, epub, &cover_image_index);
  if (err != k_ra_ok) {
    return err;
  }

  /* 3. Record metadata. */
  err = ra_rabook_set_metadata(&ctx,
                               title_off,
                               author_off,
                               language_off,
                               identifier_off,
                               cover_image_index);
  if (err != k_ra_ok) {
    return err;
  }

  /* 4. Spine chapters. */
  err = s_compile_chapters(epub, scr, &ctx);
  if (err != k_ra_ok) {
    return err;
  }

  /* 5. Finalize blob. */
  const void* blob     = nullptr;
  uint32_t    blob_len = 0U;
  err                  = ra_rabook_finalize(&ctx, &blob, &blob_len);
  if (err != k_ra_ok) {
    return err;
  }

  /* 6. Write to filesystem. */
  return ra_fs_write_file(mount, out_path, (const uint8_t*)blob, blob_len);
}
