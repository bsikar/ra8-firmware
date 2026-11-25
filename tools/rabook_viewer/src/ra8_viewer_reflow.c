/**
 * @file ra8_viewer_reflow.c
 * @brief EPUB and RABOOK reflow pagination engine for the host viewer.
 * @details Drives the shared EPUB streamer (`epub_open_streamed`) and compiled
 * `.rabook` loader (`book_open` / `book_validate`) into bounded caller-owned
 * slices, laying out every chapter with `reflow` and rasterizing requested
 * pages into RGB565 targets.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "book.h"
#include "epub.h"
#include "literata_latin1.h"
#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_decomp_limits.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_viewer_reader.h"
#include "ra8_viewer_reader_internal.h"
#include "reflow.h"

/** @brief Local constants for RABOOK container sniffing and bounded I/O. */
typedef enum : uint32_t {
  k_reflow_read_chunk_bytes = 65536U, /**< Host pread chunk size, bytes.  */
  k_reflow_container_total  = 8U,     /**< Offset of RBKC inflated_total. */
  k_reflow_raw_magic_len    = 8U,     /**< Length of "RABOOK1\0" magic.   */
} viewer_reflow_const_t;

/**
 * @brief Serve bounded descriptor reads to ::epub_open_streamed.
 * @details Adapts ::priv_viewer_pread onto the ::epub_stream_read_fn contract
 * expected by `epub`.
 * @param[in,out] ctx Bound ::viewer_file_ctx_t descriptor handle.
 * @param[in] offset Byte offset from the start of the EPUB file.
 * @param[out] buf Destination buffer writable for @p len bytes.
 * @param[in] len Exact byte count requested.
 * @param[out] out_read Receives bytes actually read.
 * @return Stream status.
 * @retval k_ra8_ok At least a bounded prefix was read.
 * @retval k_ra8_err_null_ptr A required pointer was NULL.
 * @pre @p ctx is a live ::viewer_file_ctx_t owned by the reader.
 * @pre @p buf holds at least @p len writable bytes.
 * @post `*out_read <= len` on every return.
 * @note Reentrant only across distinct descriptors.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_epub_stream_read(void* ctx, uint64_t offset, void* buf, uint32_t len, uint32_t* out_read)
{
  if (out_read == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out_read = 0U;
  if ((ctx == nullptr) || (buf == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  size_t          got   = 0U;
  const ra8_err_t error = priv_viewer_pread(ctx, offset, (uint8_t*)buf, (size_t)len, &got);
  *out_read             = (uint32_t)got;
  return error;
}

/**
 * @brief Inflate one zlib stream from a `.rabook` "RBKC" chunk via miniz.
 * @details Adapts `mz_uncompress` onto the ::book_inflate_fn callback signature
 * consumed by ::book_open.
 * @param[in] src Compressed zlib payload.
 * @param[in] src_len Compressed payload byte length.
 * @param[out] dst Destination buffer for inflated bytes.
 * @param[in] dst_cap Destination capacity in bytes.
 * @param[out] out_len Receives exact inflated byte count.
 * @return Decompression status.
 * @retval k_ra8_ok Stream inflated completely within @p dst_cap.
 * @retval k_ra8_err_null_ptr A required pointer was NULL.
 * @retval k_ra8_err_validation_failed miniz rejected the compressed stream.
 * @pre @p src and @p dst span @p src_len and @p dst_cap bytes respectively.
 * @post On success `*out_len <= dst_cap`.
 * @note Pure apart from destination buffer writes.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_inflate(const void* src, size_t src_len, void* dst, size_t dst_cap, size_t* out_len)
{
  if ((src == nullptr) || (dst == nullptr) || (out_len == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  tinfl_decompressor decomp = {};
  tinfl_init(&decomp);
  size_t             in_n   = src_len;
  size_t             out_n  = dst_cap;
  const tinfl_status status = tinfl_decompress(
    &decomp,
    (const mz_uint8*)src,
    &in_n,
    (mz_uint8*)dst,
    (mz_uint8*)dst,
    &out_n,
    (mz_uint32)(TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF));
  if ((status != TINFL_STATUS_DONE) || (in_n != src_len)) {
    return k_ra8_err_validation_failed;
  }
  *out_len = out_n;
  return k_ra8_ok;
}

/**
 * @brief Copy the complete open file into caller-bound container storage.
 * @details Rejects empty files or files larger than `file_cap` before reading
 * bounded chunks until exact EOF.
 * @param[in,out] reader Bound reader owning an open file descriptor.
 * @return Read status.
 * @retval k_ra8_ok `reflow.file_buf[0 .. reader->file.size)` holds all bytes.
 * @retval k_ra8_err_invalid_size File is empty or exceeds `reflow.file_cap`.
 * @pre `reader->file` is open and `reader->reflow.file_buf` is bound.
 * @post No heap storage is acquired.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_read_file_bytes(ra8_viewer_reader_t* reader)
{
  viewer_reflow_t* reflow = &reader->reflow;
  const uint64_t   total  = reader->source.size;
  if ((total == 0U) || (total > (uint64_t)reflow->file_cap)) {
    return k_ra8_err_invalid_size;
  }
  uint64_t offset = 0U;
  while (offset < total) {
    const uint64_t  remaining = total - offset;
    const size_t    chunk     = (remaining > (uint64_t)k_reflow_read_chunk_bytes)
                                  ? (size_t)k_reflow_read_chunk_bytes
                                  : (size_t)remaining;
    size_t          got       = 0U;
    const ra8_err_t error =
      priv_viewer_pread(&reader->source, offset, &reflow->file_buf[offset], chunk, &got);
    if (error != k_ra8_ok) {
      return error;
    }
    if (got == 0U) {
      return k_ra8_err_invalid_size;
    }
    offset += (uint64_t)got;
  }
  return k_ra8_ok;
}

/**
 * @brief Open and validate a streamed EPUB document.
 * @details Wraps `reader->file` in an ::epub_stream_media_t descriptor, parses
 * `container.xml` and the OPF spine via ::epub_open_streamed, and rejects books
 * without readable chapters.
 * @param[in,out] reader Bound reader with EPUB slice prepared.
 * @return Open status.
 * @retval k_ra8_ok EPUB spine is open and has at least one chapter.
 * @retval k_ra8_err_validation_failed EPUB had zero spine chapters.
 * @pre `reader->file` is open and `reader->reflow.epub` is bound.
 * @post `reflow.chapter_count > 0` on success.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_open_epub(ra8_viewer_reader_t* reader)
{
  viewer_reflow_t*          reflow = &reader->reflow;
  const epub_stream_media_t media  = {
    .read = internal_epub_stream_read,
    .ctx  = &reader->source,
    .size = reader->source.size,
  };
  ra8_err_t error = epub_open_streamed(&media, nullptr, reflow->epub);
  if (error != k_ra8_ok) {
    return error;
  }
  uint16_t chapters = 0U;
  error             = epub_get_chapter_count(reflow->epub, &chapters);
  if ((error == k_ra8_ok) && (chapters == 0U)) {
    error = k_ra8_err_validation_failed;
  }
  if (error == k_ra8_ok) {
    reflow->chapter_count = (uint32_t)chapters;
  }
  return error;
}

/**
 * @brief Open and validate a compiled `.rabook` container or flat blob.
 * @details Supports compressed "RBKC" containers via ::book_open after checking
 * declared size against ::ra8_decomp_check_declared, as well as validated flat
 * "RABOOK1" blobs.
 * @param[in,out] reader Bound reader with `.rabook` slices prepared.
 * @return Validation or open status.
 * @retval k_ra8_ok Validated `book_base` and `chapter_count` are stored.
 * @retval k_ra8_err_invalid_size File exceeds workspace bounds.
 * @retval k_ra8_err_validation_failed Magic, header, or zero chapters failed.
 * @pre `reader->file` is open and `.rabook` slices are bound.
 * @post `reflow.book_base != nullptr` and `reflow.chapter_count > 0` on success.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_open_rabook(ra8_viewer_reader_t* reader)
{
  viewer_reflow_t* reflow = &reader->reflow;
  ra8_err_t        error  = internal_read_file_bytes(reader);
  if (error != k_ra8_ok) {
    return error;
  }
  const size_t file_len = (size_t)reader->source.size;
  const void*  base     = nullptr;
  size_t       out_size = 0U;
  if ((file_len >= (size_t)k_book_container_header_len) &&
      (memcmp(reflow->file_buf, "RBKC", (size_t)k_book_container_magic_len) == 0)) {
    uint64_t declared_total = 0U;
    (void)memcpy(&declared_total,
                 &reflow->file_buf[k_reflow_container_total],
                 sizeof(declared_total));
    error = ra8_decomp_check_declared(&reader->limits, reader->source.size, declared_total);
    if ((error == k_ra8_ok) &&
        ((declared_total == 0U) || (declared_total > (uint64_t)reflow->book_cap))) {
      error = k_ra8_err_invalid_size;
    }
    if (error == k_ra8_ok) {
      error = book_open(reflow->file_buf,
                        file_len,
                        internal_inflate,
                        reflow->book_buf,
                        reflow->book_cap,
                        &base,
                        &out_size);
    }
  } else {
    error    = book_validate(reflow->file_buf, file_len);
    base     = reflow->file_buf;
    out_size = file_len;
  }
  if (error != k_ra8_ok) {
    return error;
  }
  const book_header_t* hdr = book_header(base);
  if (hdr->chapter_count == 0U) {
    return k_ra8_err_validation_failed;
  }
  reflow->book_base     = base;
  reflow->book_size     = out_size;
  reflow->chapter_count = hdr->chapter_count;
  return k_ra8_ok;
}

/**
 * @brief Extract one chapter as XHTML bytes into `reflow.xhtml`.
 * @details Loads directly from ZIP via ::epub_load_chapter or serializes the
 * pre-parsed DOM via ::book_chapter_to_xhtml.
 * @param[in,out] reader Open reflow reader.
 * @param[in] chapter Chapter index below `chapter_count`.
 * @param[out] out_len Exact extracted XHTML byte count.
 * @return Extraction status.
 * @retval k_ra8_ok Non-empty chapter XHTML occupies `reflow.xhtml[0..*out_len)`.
 * @retval k_ra8_err_validation_failed Chapter serialized to zero bytes.
 * @pre @p chapter < `reader->reflow.chapter_count`.
 * @pre @p out_len is writable.
 * @post `reflow.xhtml` contains @p *out_len bytes on success.
 * @note Not thread-safe; reuses shared chapter XHTML slice.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_load_chapter(ra8_viewer_reader_t* reader, uint32_t chapter, size_t* out_len)
{
  viewer_reflow_t* reflow = &reader->reflow;
  *out_len                = 0U;
  const ra8_err_t error   = reflow->is_epub ? epub_load_chapter(reflow->epub,
                                                                (uint16_t)chapter,
                                                                reflow->xhtml,
                                                                reflow->xhtml_cap,
                                                                out_len)
                                            : book_chapter_to_xhtml(reflow->book_base,
                                                                    chapter,
                                                                    (char*)reflow->xhtml,
                                                                    reflow->xhtml_cap,
                                                                    out_len);
  if ((error == k_ra8_ok) && (*out_len == 0U)) {
    return k_ra8_err_validation_failed;
  }
  return error;
}

/**
 * @brief Append all paginated pages of one laid-out chapter into the book map.
 * @details Maps each local page to global viewer tile dimensions and stops with
 * `k_ra8_err_no_mem` if the caller's page slice fills.
 * @param[in,out] reader Open reflow reader.
 * @param[in] chapter Spine chapter index just laid out.
 * @param[in] local_pages Total pages produced by ::reflow_layout_chapter.
 * @return Append status.
 * @retval k_ra8_ok All local pages fit in `pages[]`.
 * @retval k_ra8_err_validation_failed Chapter produced zero pages.
 * @retval k_ra8_err_no_mem Book exceeded `tile_cap`.
 * @pre `local_pages` reflects the latest ::reflow_layout_chapter output.
 * @post `reflow.page_count` and `reader->tile_n` advance by @p local_pages.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_append_chapter_pages(ra8_viewer_reader_t* reader, uint32_t chapter, uint32_t local_pages)
{
  if (local_pages == 0U) {
    return k_ra8_err_validation_failed;
  }
  viewer_reflow_t* reflow = &reader->reflow;
  if ((uint64_t)reflow->page_count + (uint64_t)local_pages > (uint64_t)reader->tile_cap) {
    return k_ra8_err_no_mem;
  }
  for (uint32_t local = 0U; local < local_pages; ++local) {
    const uint32_t idx    = reflow->page_count++;
    reflow->pages[idx]    = (viewer_reflow_page_t){.chapter = chapter, .local_page = local};
    reader->tile_wpx[idx] = (uint32_t)k_ra8_viewer_fb_width;
    reader->tile_hpx[idx] = (uint32_t)k_ra8_viewer_fb_height;
  }
  reader->tile_n         = reflow->page_count;
  reflow->loaded_chapter = chapter;
  return k_ra8_ok;
}

/**
 * @brief Initialize `reflow` and paginate every spine chapter up front.
 * @details Ensures deterministic page count, viewport dimensions, and cached last
 * chapter on open.
 * @param[in,out] reader Bound reader with open EPUB or RABOOK source.
 * @return Initialization or layout status.
 * @retval k_ra8_ok All chapters paginated with at least one page total.
 * @pre Open document recorded valid `chapter_count > 0`.
 * @post `reader->tile_n == reader->reflow.page_count >= 1` on success.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_paginate_book(ra8_viewer_reader_t* reader)
{
  viewer_reflow_t* reflow = &reader->reflow;
  ra8_err_t        error  = reflow_init((uint16_t)k_ra8_viewer_fb_width,
                                        (uint16_t)k_ra8_viewer_fb_height,
                                        g_ra8_font_literata_latin1,
                                        (size_t)g_ra8_font_literata_latin1_len,
                                        (uint16_t)k_viewer_reflow_font_px,
                                        (uint32_t)k_viewer_reflow_body_color,
                                        (uint32_t)k_viewer_reflow_link_color,
                                        reflow->engine);
  if (error != k_ra8_ok) {
    return error;
  }
  reflow->page_count     = 0U;
  reflow->loaded_chapter = UINT32_MAX;
  reader->tile_n         = 0U;
  for (uint32_t chapter = 0U; chapter < reflow->chapter_count; ++chapter) {
    size_t xhtml_len = 0U;
    error            = internal_load_chapter(reader, chapter, &xhtml_len);
    if (error != k_ra8_ok) {
      return error;
    }
    uint32_t local_pages = 0U;
    error = reflow_layout_chapter(reflow->engine, reflow->xhtml, xhtml_len, &local_pages);
    if (error != k_ra8_ok) {
      return error;
    }
    error = internal_append_chapter_pages(reader, chapter, local_pages);
    if (error != k_ra8_ok) {
      return error;
    }
  }
  return (reflow->page_count == 0U) ? k_ra8_err_validation_failed : k_ra8_ok;
}

/**
 * @brief Render one reflow page into the given RGB565 surface.
 * @details Reloads and lays out the target chapter only when the caller steps
 * across a chapter boundary, clears the surface white, and draws glyphs.
 * @param[in,out] reader Open reflow reader.
 * @param[in] page Global book page index.
 * @param[out] pixels Destination RGB565 pixel buffer.
 * @param[in] width Target width in pixels.
 * @param[in] height Target height in pixels.
 * @return Render status.
 * @retval k_ra8_ok Page rasterized completely into @p pixels.
 * @retval k_ra8_err_out_of_range @p page >= `reflow.page_count`.
 * @pre @p pixels spans at least `width * height * sizeof(uint16_t)` bytes.
 * @post Global graphics target was bound to @p pixels during draw.
 * @note Not thread-safe; drives shared reflow and graphics state.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_render_surface(ra8_viewer_reader_t* reader,
                                                      uint32_t             page,
                                                      uint16_t*            pixels,
                                                      uint16_t             width,
                                                      uint16_t             height)
{
  viewer_reflow_t* reflow = &reader->reflow;
  if (page >= reflow->page_count) {
    return k_ra8_err_out_of_range;
  }
  const viewer_reflow_page_t target = reflow->pages[page];
  ra8_err_t                  error  = k_ra8_ok;
  if (reflow->loaded_chapter != target.chapter) {
    size_t xhtml_len = 0U;
    error            = internal_load_chapter(reader, target.chapter, &xhtml_len);
    if (error == k_ra8_ok) {
      uint32_t local_pages = 0U;
      error = reflow_layout_chapter(reflow->engine, reflow->xhtml, xhtml_len, &local_pages);
    }
    if (error == k_ra8_ok) {
      reflow->loaded_chapter = target.chapter;
    }
  }
  if (error == k_ra8_ok) {
    error = ra8_gfx_init(pixels, width, height, k_ra8_gfx_format_rgb565);
  }
  if (error == k_ra8_ok) {
    error = ra8_gfx_clear((uint32_t)k_viewer_background);
  }
  if (error == k_ra8_ok) {
    error = reflow_render_page(reflow->engine, target.local_page, pixels);
  }
  return error;
}

ra8_err_t priv_viewer_open_reflow(ra8_viewer_reader_t* reader)
{
  viewer_reflow_t* reflow = &reader->reflow;
  reflow->book_base       = nullptr;
  reflow->book_size       = 0U;
  reflow->chapter_count   = 0U;
  reflow->page_count      = 0U;
  reflow->loaded_chapter  = UINT32_MAX;
  ra8_err_t error = reflow->is_epub ? internal_open_epub(reader) : internal_open_rabook(reader);
  if (error == k_ra8_ok) {
    error = internal_paginate_book(reader);
  }
  if (error != k_ra8_ok) {
    priv_viewer_close_reflow(reader);
  }
  return error;
}

void priv_viewer_close_reflow(ra8_viewer_reader_t* reader)
{
  if (reader == nullptr) {
    return;
  }
  viewer_reflow_t* reflow = &reader->reflow;
  if ((reflow->engine != nullptr) && (reflow->engine->in_use != 0U)) {
    (void)reflow_close(reflow->engine);
  }
  if (reflow->is_epub && (reflow->epub != nullptr) && (reflow->epub->in_use != 0U)) {
    (void)epub_close(reflow->epub);
  }
  reflow->book_base      = nullptr;
  reflow->book_size      = 0U;
  reflow->chapter_count  = 0U;
  reflow->page_count     = 0U;
  reflow->loaded_chapter = UINT32_MAX;
}

ra8_err_t priv_viewer_render_reflow(ra8_viewer_reader_t* reader, uint32_t page)
{
  reader->rt565 = reader->fb;
  reader->rt_w  = (uint32_t)k_ra8_viewer_fb_width;
  reader->rt_h  = (uint32_t)k_ra8_viewer_fb_height;
  return internal_render_surface(reader,
                                 page,
                                 reader->fb,
                                 (uint16_t)k_ra8_viewer_fb_width,
                                 (uint16_t)k_ra8_viewer_fb_height);
}

ra8_err_t priv_viewer_tile_reflow(ra8_viewer_reader_t* reader,
                                  uint32_t             index,
                                  uint16_t*            pixels,
                                  size_t               pixel_bytes,
                                  uint32_t*            width,
                                  uint32_t*            height)
{
  const uint32_t render_width  = reader->tile_wpx[index];
  const uint32_t render_height = reader->tile_hpx[index];
  const uint64_t required =
    (uint64_t)render_width * (uint64_t)render_height * (uint64_t)sizeof(uint16_t);
  if ((render_width == 0U) || (render_height == 0U) || (required > (uint64_t)pixel_bytes) ||
      (render_width > UINT16_MAX) || (render_height > UINT16_MAX)) {
    return k_ra8_err_invalid_size;
  }
  ra8_err_t error =
    internal_render_surface(reader, index, pixels, (uint16_t)render_width, (uint16_t)render_height);
  const ra8_err_t restored = ra8_gfx_init(reader->fb,
                                          (uint16_t)k_ra8_viewer_fb_width,
                                          (uint16_t)k_ra8_viewer_fb_height,
                                          k_ra8_gfx_format_rgb565);
  if ((error == k_ra8_ok) && (restored != k_ra8_ok)) {
    error = restored;
  }
  if (error == k_ra8_ok) {
    *width  = render_width;
    *height = render_height;
  }
  return error;
}
