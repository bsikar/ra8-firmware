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
 * @brief Decode one raw image, transcode to 4-bpp gray, add to builder.
 * @details Binds @p scr->img_arena before calling stb_image and unbinds on
 *          return.  The encoded nibbles are written into @p scr->image_raw
 *          (reusing the buffer that held the raw bytes).
 * @return Image index on success, or @ref k_ra_book_nil on any failure.
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

  const uint8_t* gray_src = pixels;
  if (ow != (uint16_t)sw || oh != (uint16_t)sh) {
    if ((uint32_t)ow * oh > scr->gray_cap) {
      ra_log_error(s_tag, "gray scratch too small for downscale");
      stbi_image_free(pixels);
      ra_img_arena_unbind();
      return k_ra_book_nil;
    }
    ra_err_t ds_err =
      ra_rabook_gray4_downscale(pixels, (uint16_t)sw, (uint16_t)sh, scr->gray, ow, oh);
    if (ds_err != k_ra_ok) {
      stbi_image_free(pixels);
      ra_img_arena_unbind();
      return k_ra_book_nil;
    }
    gray_src = scr->gray;
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
 * @return Pointer to the interned title string (empty string if no match).
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

/* -------------------------------------------------------------------------- */
/* Public API */
/* -------------------------------------------------------------------------- */

ra_err_t ra_rabook_compile_from_epub(ra_epub_book_t*                     epub,
                                     const ra_rabook_buffers_t*          bufs,
                                     const ra_rabook_pipeline_scratch_t* scr,
                                     ra_fs_mount_t*                      mount,
                                     const char*                         out_path)
{
  RA_CHECK_NULL_PTR(epub, s_tag, "epub");
  RA_CHECK_NULL_PTR(bufs, s_tag, "bufs");
  RA_CHECK_NULL_PTR(scr, s_tag, "scr");
  RA_CHECK_NULL_PTR(mount, s_tag, "mount");
  RA_CHECK_NULL_PTR(out_path, s_tag, "out_path");

  ra_rabook_ctx_t ctx = {};
  ra_err_t        err = ra_rabook_compile_init(&ctx, bufs);
  if (err != k_ra_ok) {
    return err;
  }

  /* 1. Metadata */
  ra_epub_metadata_t meta = {};
  err                     = ra_epub_get_metadata(epub, &meta);
  if (err != k_ra_ok) {
    return err;
  }
  uint32_t title_off      = ra_rabook_intern(&ctx, meta.title);
  uint32_t author_off     = ra_rabook_intern(&ctx, meta.author);
  uint32_t language_off   = ra_rabook_intern(&ctx, meta.language);
  uint32_t identifier_off = ra_rabook_intern(&ctx, meta.identifier);

  /* 2. Cover image */
  uint32_t cover_image_index = k_ra_book_nil;
  size_t   raw_len           = 0U;
  err = ra_epub_get_cover_image(epub, scr->image_raw, scr->image_cap, &raw_len);
  if (err == k_ra_ok) {
    uint32_t cover_id_off = ra_rabook_intern(&ctx, epub->cover_path);
    cover_image_index     = s_transcode_image(&ctx, scr, cover_id_off, raw_len);
    /* k_ra_book_nil is the documented "no cover" sentinel; allow it. */
  } else if (err != k_ra_err_not_found) {
    return err;
  }

  err = ra_rabook_set_metadata(&ctx,
                               title_off,
                               author_off,
                               language_off,
                               identifier_off,
                               cover_image_index);
  if (err != k_ra_ok) {
    return err;
  }

  /* 3. Spine chapters */
  uint16_t chapter_count = 0U;
  err                    = ra_epub_get_chapter_count(epub, &chapter_count);
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

    err = ra_rabook_xml_parse_chapter(scr->xhtml, got_len, &ctx, ch_href, ch_title);
    if (err != k_ra_ok) {
      return err;
    }
  }

  /* 4. Finalize blob */
  const void* blob     = nullptr;
  uint32_t    blob_len = 0U;
  err                  = ra_rabook_finalize(&ctx, &blob, &blob_len);
  if (err != k_ra_ok) {
    return err;
  }

  /* 5. Write to filesystem */
  return ra_fs_write_file(mount, out_path, (const uint8_t*)blob, blob_len);
}
