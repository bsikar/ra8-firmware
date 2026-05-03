/**
 * @file ra_epub_chapter.c
 * @brief Chapter / metadata / cover / glyph accessors for the EPUB reader.
 *
 * @details
 * The functions in this TU all read against an already-open
 * `ra_epub_book_t`:
 *
 *   - `ra_epub_get_chapter_count()` -- size of the spine.
 *   - `ra_epub_load_chapter()`      -- extract one XHTML body via miniz.
 *   - `ra_epub_get_metadata()`      -- title / author / language strings.
 *   - `ra_epub_get_cover_image()`   -- raw bytes of the cover, decoded by
 *                                      the caller via stb_image.
 *   - `ra_epub_set_font()`          -- attach a TTF blob.
 *   - `ra_epub_render_glyph()`      -- rasterise a code point via
 *                                      stb_truetype into an alpha-8 buffer.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / EPUB]
 * {World: NS}
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "miniz.h"

/* stb_truetype is a single-header library; the implementation lives
 * in libs/third_party/stb/stb_truetype_impl.c so that clang-tidy's
 * third_party path exclusion keeps stb's dense magic-number / huge-
 * function code out of the lint pass. We include only declarations
 * here. */
#include "ra_epub.h"
#include "ra_epub_internal.h"
#include "ra_err.h"
#include "stb_truetype.h"

/* ---------------------------------------------------------------------------
 * Internal limits.
 * ---------------------------------------------------------------------------
 */

/**
 * @enum ra_epub_chapter_internal_t
 * @brief Implementation-only sizing constants.
 */
typedef enum : uint16_t {
  k_ra_epub_min_font_bytes = 16, /**< Smallest TTF blob we'll accept. */
} ra_epub_chapter_internal_t;

/* ---------------------------------------------------------------------------
 * Helpers.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Pure glyph-dim-invalid predicate -- see header for full contract.
 * @details Promoted helper so the line-225 OR can be driven under MC/DC.
 * @param[in] w Glyph bbox width.
 * @param[in] h Glyph bbox height.
 * @return Boolean reject predicate.
 * @retval true  Caller returns validation-failed.
 * @retval false Both dimensions OK.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool ra_epub_internal_glyph_dim_invalid(int w, int h)
{
  return (w < 0) || (h < 0);
}

/**
 * @brief Pure book-not-ready predicate -- see header for full contract.
 * @details Promoted helper so the line-300/369 OR can be driven under MC/DC.
 * @param[in] in_use             Book "in_use" byte.
 * @param[in] zip_archive_active Book "zip_archive_active" byte.
 * @return Boolean reject predicate.
 * @retval true  Caller returns not-initialized.
 * @retval false Book is ready.
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return depends solely on inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
bool ra_epub_internal_book_not_ready(uint8_t in_use, uint8_t zip_archive_active)
{
  return (in_use == 0U) || (zip_archive_active == 0U);
}

/**
 * @brief Concatenate `dir` + `name` into `dst`, NUL-terminated.
 *
 * @details Joins two NUL-terminated path components into the
 *          caller-supplied buffer with bounded length.
 * @param[in]  dir  Directory prefix (may be NULL).
 * @param[in]  name Name suffix (may be NULL).
 * @param[out] dst  Destination buffer.
 * @param[in]  cap  Capacity of @p dst in bytes.
 * @pre cap == 0 implies dst may be NULL.
 * @pre cap > 0 implies dst is non-NULL and writeable.
 * @post dst is NUL-terminated when cap > 0.
 * @post No more than (cap - 1) bytes written from inputs.
 * @note Not thread-safe.
 * @since 0.1.0
 */
void ra_epub_internal_join_path(const char* dir, const char* name, char* dst, size_t cap)
{
  if (dst == NULL || cap == 0U) {
    return;
  }
  dst[0]     = '\0';
  size_t off = 0U;
  if (dir != NULL) {
    while (off + 1U < cap && dir[off] != '\0') {
      dst[off] = dir[off];
      ++off;
    }
  }
  if (name != NULL) {
    size_t i = 0U;
    while (off + 1U < cap && name[i] != '\0') {
      dst[off] = name[i];
      ++off;
      ++i;
    }
  }
  dst[off] = '\0';
}

/**
 * @brief Locate-then-extract a file from the open zip into a caller buffer.
 *
 * Looks up `prefixed_path` first; on miss falls back to `bare_path`
 * (some EPUBs store hrefs without the OPF directory prefix).
 *
 * @details See implementation.
 * @param[in] zip See implementation.
 * @param[in] prefixed_path See implementation.
 * @param[in] bare_path See implementation.
 * @param[in] out_buf See implementation.
 * @param[in] max_len See implementation.
 * @param[in] got_len See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t priv_locate_extract(mz_zip_archive* zip,
                                    const char*     prefixed_path,
                                    const char*     bare_path,
                                    uint8_t*        out_buf,
                                    size_t          max_len,
                                    size_t*         got_len)
{
  int32_t file_idx = mz_zip_reader_locate_file(zip, prefixed_path, NULL, 0U);
  if (file_idx < 0) {
    file_idx = mz_zip_reader_locate_file(zip, bare_path, NULL, 0U);
  }
  if (file_idx < 0) {
    return k_ra_err_not_found;
  }
  mz_zip_archive_file_stat st;
  if (mz_zip_reader_file_stat(zip, (mz_uint)file_idx, &st) == MZ_FALSE) {
    return k_ra_err_validation_failed;
  }
  if ((size_t)st.m_uncomp_size > max_len) {
    return k_ra_err_no_mem;
  }
  if (mz_zip_reader_extract_to_mem(zip, (mz_uint)file_idx, out_buf, max_len, 0U) == MZ_FALSE) {
    return k_ra_err_validation_failed;
  }
  *got_len = (size_t)st.m_uncomp_size;
  return k_ra_ok;
}

/**
 * @brief Initialise an stbtt_fontinfo from the book's font blob.
 *
 * @return k_ra_ok on success; k_ra_err_validation_failed on a
 *         malformed / non-TTF blob.
 *
 * @details See implementation.
 * @param[in] book See implementation.
 * @param[in] out_font See implementation.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t priv_font_init(const ra_epub_book_t* book, stbtt_fontinfo* out_font)
{
  /* Guard against malformed font blobs: stbtt_GetFontOffsetForIndex
   * returns -1 for non-TTF input, and stbtt_InitFont dereferences past
   * the buffer when handed that offset. */
  const int32_t offset = stbtt_GetFontOffsetForIndex(book->font_data, 0);
  if (offset < 0) {
    return k_ra_err_validation_failed;
  }
  if (stbtt_InitFont(out_font, book->font_data, offset) == 0) {
    return k_ra_err_validation_failed;
  }
  return k_ra_ok;
}

/**
 * @brief Rasterise the code point and copy into the caller buffer.
 *
 * Splits the second half of `ra_epub_render_glyph` out so that
 * function stays under the NASA-Rule-4 statement budget enforced by
 * clang-tidy (`readability-function-size`).
 *
 * @details See implementation.
 * @param[in] font See implementation.
 * @param[in] codepoint See implementation.
 * @param[in] font_size See implementation.
 * @param[in] out_bitmap See implementation.
 * @param[in] max_pixels See implementation.
 * @param[in] out_w See implementation.
 * @param[in] out_h See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t priv_render_into(const stbtt_fontinfo* font,
                                 int32_t               codepoint,
                                 float                 font_size,
                                 uint8_t*              out_bitmap,
                                 size_t                max_pixels,
                                 uint32_t*             out_w,
                                 uint32_t*             out_h)
{
  /* No-allocation glyph rasterisation. stbtt_GetCodepointBitmap()
   * calls STBTT_malloc internally; instead use the two-step "Box"
   * + "Make" path that writes into the caller's buffer. NASA Rule 3
   * compliance. */
  const float scale = stbtt_ScaleForPixelHeight(font, font_size);
  int         x0    = 0;
  int         y0    = 0;
  int         x1    = 0;
  int         y1    = 0;
  stbtt_GetCodepointBitmapBox(font, codepoint, scale, scale, &x0, &y0, &x1, &y1);
  const int w = x1 - x0;
  const int h = y1 - y0;
  if (ra_epub_internal_glyph_dim_invalid(w, h)) {
    return k_ra_err_validation_failed;
  }
  const size_t total = (size_t)w * (size_t)h;
  if (total > max_pixels) {
    return k_ra_err_no_mem;
  }
  if (total > 0U) {
    /* Stride = w (tightly packed alpha-8). MakeCodepointBitmap writes
     * directly into out_bitmap with no malloc. */
    stbtt_MakeCodepointBitmap(font, out_bitmap, w, h, w, scale, scale, codepoint);
  }
  *out_w = (uint32_t)w;
  *out_h = (uint32_t)h;
  return k_ra_ok;
}

/* ---------------------------------------------------------------------------
 * Public API.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Implementation of ra_epub_get_chapter_count (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] book See implementation.
 * @param[in] out_count See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_epub_get_chapter_count(const ra_epub_book_t* book, uint16_t* out_count)
{
  if (book == NULL || out_count == NULL) {
    return k_ra_err_null_ptr;
  }
  if (book->in_use == 0U) {
    return k_ra_err_not_initialized;
  }
  *out_count = book->chapter_count;
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_epub_load_chapter (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] book See implementation.
 * @param[in] idx See implementation.
 * @param[in] out_xhtml See implementation.
 * @param[in] max_len See implementation.
 * @param[in] got_len See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_epub_load_chapter(ra_epub_book_t* book,
                              uint16_t        idx,
                              uint8_t*        out_xhtml,
                              size_t          max_len,
                              size_t*         got_len)
{
  if (book == NULL || out_xhtml == NULL || got_len == NULL) {
    return k_ra_err_null_ptr;
  }
  *got_len = 0U;
  if (ra_epub_internal_book_not_ready(book->in_use, book->zip_archive_active)) {
    return k_ra_err_not_initialized;
  }
  if (max_len == 0U) {
    return k_ra_err_invalid_size;
  }
  if (idx >= book->chapter_count) {
    return k_ra_err_out_of_range;
  }

  char full_path[k_ra_epub_max_path_len];
  ra_epub_internal_join_path(book->opf_dir, book->chapter_paths[idx], full_path, sizeof(full_path));

  void* const     zip_storage = &book->zip_archive_storage[0];
  mz_zip_archive* zip         = (mz_zip_archive*)zip_storage;
  return priv_locate_extract(zip, full_path, book->chapter_paths[idx], out_xhtml, max_len, got_len);
}

/**
 * @brief Implementation of ra_epub_get_metadata (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] book See implementation.
 * @param[in] out_meta See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_epub_get_metadata(const ra_epub_book_t* book, ra_epub_metadata_t* out_meta)
{
  if (book == NULL || out_meta == NULL) {
    return k_ra_err_null_ptr;
  }
  if (book->in_use == 0U) {
    return k_ra_err_not_initialized;
  }
  out_meta->title    = book->title;
  out_meta->author   = book->author;
  out_meta->language = book->language;
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_epub_get_cover_image (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] book See implementation.
 * @param[in] out_buf See implementation.
 * @param[in] max_len See implementation.
 * @param[in] got_len See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t
ra_epub_get_cover_image(ra_epub_book_t* book, uint8_t* out_buf, size_t max_len, size_t* got_len)
{
  if (book == NULL || out_buf == NULL || got_len == NULL) {
    return k_ra_err_null_ptr;
  }
  *got_len = 0U;
  if (ra_epub_internal_book_not_ready(book->in_use, book->zip_archive_active)) {
    return k_ra_err_not_initialized;
  }
  if (max_len == 0U) {
    return k_ra_err_invalid_size;
  }
  if (book->cover_path[0] == '\0') {
    return k_ra_err_not_found;
  }

  char full_path[k_ra_epub_max_path_len];
  ra_epub_internal_join_path(book->opf_dir, book->cover_path, full_path, sizeof(full_path));

  void* const     zip_storage = &book->zip_archive_storage[0];
  mz_zip_archive* zip         = (mz_zip_archive*)zip_storage;
  return priv_locate_extract(zip, full_path, book->cover_path, out_buf, max_len, got_len);
}

/**
 * @brief Implementation of ra_epub_set_font (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] book See implementation.
 * @param[in] font_data See implementation.
 * @param[in] font_size See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_epub_set_font(ra_epub_book_t* book, const uint8_t* font_data, size_t font_size)
{
  if (book == NULL || font_data == NULL) {
    return k_ra_err_null_ptr;
  }
  if (book->in_use == 0U) {
    return k_ra_err_not_initialized;
  }
  if (font_size < (size_t)k_ra_epub_min_font_bytes) {
    return k_ra_err_invalid_size;
  }
  book->font_data = font_data;
  book->font_size = font_size;
  return k_ra_ok;
}

/**
 * @brief Implementation of ra_epub_render_glyph (see header for full contract).
 * @details See the public header for the documented contract; this definition implements it.
 * @param[in] book See implementation.
 * @param[in] codepoint See implementation.
 * @param[in] font_size See implementation.
 * @param[in] out_bitmap See implementation.
 * @param[in] max_pixels See implementation.
 * @param[in] out_w See implementation.
 * @param[in] out_h See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
ra_err_t ra_epub_render_glyph(const ra_epub_book_t* book,
                              int32_t               codepoint,
                              float                 font_size,
                              uint8_t*              out_bitmap,
                              size_t                max_pixels,
                              uint32_t*             out_w,
                              uint32_t*             out_h)
{
  if (book == NULL || out_bitmap == NULL || out_w == NULL || out_h == NULL) {
    return k_ra_err_null_ptr;
  }
  *out_w = 0U;
  *out_h = 0U;
  if (book->in_use == 0U || book->font_data == NULL) {
    return k_ra_err_not_initialized;
  }
  if (font_size <= 0.0F) {
    return k_ra_err_invalid_arg;
  }

  stbtt_fontinfo font;
  ra_err_t       err = priv_font_init(book, &font);
  if (err != k_ra_ok) {
    return err;
  }
  return priv_render_into(&font, codepoint, font_size, out_bitmap, max_pixels, out_w, out_h);
}
