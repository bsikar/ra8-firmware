/**
 * @file ra8_viewer_reader.c
 * @brief Host-side reader core: file-backed ra8_comic paging into an RGB565 fb.
 *
 * @details
 * Implements ra8_viewer_reader.h. A document is opened over a `pread`-style
 * seek+read callback (::viewer_read) so the firmware's demand-paged ra8_comic
 * engine streams it exactly as it does off the SD card on the board. Each page is
 * extracted to an encoded-image scratch buffer and handed to ra8_reflow's
 * `ra8_img_decode_blit`, which decodes it through stb_image (backed by a caller
 * bump arena, no heap inside the decode) and blits it -- scaled to fit and
 * centred -- into an owned RGB565 framebuffer bound to ra8_gfx.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "ra8_viewer_reader.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ra8_comic.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_reflow_image.h"

/**
 * @enum ra8_viewer_budget_t
 * @brief Fixed capacities for the reader's owned buffers (no magic numbers).
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_viewer_page_cap    = 8192U,                /**< Max pages in the sorted index. */
  k_viewer_name_cap    = 512U * 1024U,         /**< Page-name arena, bytes.        */
  k_viewer_arena_bytes = 160U * 1024U * 1024U, /**< stb_image decode scratch.    */
} ra8_viewer_budget_t;

/**
 * @enum ra8_viewer_color_t
 * @brief Framebuffer colours as 0x00RRGGBB words for ra8_gfx.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_viewer_bg = 0x00FFFFFFU, /**< Page background: white (e-ink paper). */
} ra8_viewer_color_t;

/**
 * @struct viewer_file_ctx_t
 * @brief Backing context for the seek+read callback: an open file and its size.
 * @since 0.1.0
 */
typedef struct {
  FILE*    fp;   /**< Open document stream.   */
  uint64_t size; /**< Document length, bytes. */
} viewer_file_ctx_t;

/**
 * @struct ra8_viewer_reader
 * @brief One open document: file backing, comic engine state, and owned buffers.
 * @since 0.1.0
 */
struct ra8_viewer_reader {
  viewer_file_ctx_t file;      /**< File backing for the read callback.    */
  ra8_comic_t       comic;     /**< Comic engine (CBZ / CBR).              */
  ra8_comic_page_t* pages;     /**< Page index (k_viewer_page_cap entries).*/
  char*             names;     /**< Page-name arena.                       */
  uint16_t*         fb;        /**< RGB565 framebuffer (owned).            */
  uint8_t*          page_buf;  /**< Encoded-image scratch (grows on demand).*/
  size_t            page_cap;  /**< Capacity of @ref page_buf, bytes.      */
  uint8_t*          arena_mem; /**< Backing store for the decode arena.    */
};

/**
 * @brief Seek+read callback over the open document (matches ra8_comic_read_fn).
 * @param[in]  ctx    A ::viewer_file_ctx_t.
 * @param[in]  offset Absolute byte offset.
 * @param[out] buf    Destination buffer.
 * @param[in]  len    Bytes requested.
 * @return Bytes read (0 at EOF or on error).
 * @since 0.1.0
 */
static size_t viewer_read(void* ctx, uint64_t offset, void* buf, size_t len)
{
  viewer_file_ctx_t* fc = (viewer_file_ctx_t*)ctx;
  if ((fc == nullptr) || (fc->fp == nullptr) || (buf == nullptr)) {
    return 0U;
  }
  if (offset >= fc->size) {
    return 0U;
  }
  if (fseeko(fc->fp, (off_t)offset, SEEK_SET) != 0) {
    return 0U;
  }
  return fread(buf, 1U, len, fc->fp);
}

/**
 * @brief Free every owned buffer of @p r and the handle itself.
 * @param[in,out] r Reader to release (NULL is ignored).
 * @since 0.1.0
 */
static void viewer_free(ra8_viewer_reader_t* r)
{
  if (r == nullptr) {
    return;
  }
  if (r->comic.kind != k_ra8_comic_kind_none) {
    (void)ra8_comic_close(&r->comic);
  }
  if (r->file.fp != nullptr) {
    (void)fclose(r->file.fp);
  }
  free(r->arena_mem);
  free(r->page_buf);
  free(r->fb);
  free(r->names);
  free(r->pages);
  free(r);
}

/**
 * @brief Test whether @p path ends (case-insensitively) with @p ext.
 * @param[in] path Path to test.
 * @param[in] ext  Lowercase extension including the dot (e.g. ".cbz").
 * @return true when @p path ends with @p ext.
 * @since 0.1.0
 */
static bool viewer_ends_with(const char* path, const char* ext)
{
  const size_t pl = strlen(path);
  const size_t el = strlen(ext);
  if (pl < el) {
    return false;
  }
  const char* tail = path + (pl - el);
  for (size_t i = 0U; i < el; ++i) {
    char c = tail[i];
    if ((c >= 'A') && (c <= 'Z')) {
      c = (char)(c + ('a' - 'A'));
    }
    if (c != ext[i]) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Whether @p path names a comic archive by extension (`.cbz`/`.cbr`/`.cbt`).
 * @param[in] path Path to classify.
 * @return true for a comic-archive extension.
 * @since 0.1.0
 */
static bool viewer_is_comic_path(const char* path)
{
  return viewer_ends_with(path, ".cbz") || viewer_ends_with(path, ".cbr") ||
         viewer_ends_with(path, ".cbt");
}

/**
 * @brief Allocate the reader handle and all fixed-size owned buffers.
 * @param[out] out Receives the allocated reader on success.
 * @return ra8_err_t; ::k_ra8_err_no_mem on any allocation failure.
 * @since 0.1.0
 */
static ra8_err_t viewer_alloc(ra8_viewer_reader_t** out)
{
  ra8_viewer_reader_t* r = (ra8_viewer_reader_t*)calloc(1U, sizeof(*r));
  if (r == nullptr) {
    return k_ra8_err_no_mem;
  }
  const size_t fb_pixels = (size_t)k_ra8_viewer_fb_width * (size_t)k_ra8_viewer_fb_height;
  r->pages               = (ra8_comic_page_t*)calloc((size_t)k_viewer_page_cap, sizeof(*r->pages));
  r->names               = (char*)calloc((size_t)k_viewer_name_cap, sizeof(char));
  r->fb                  = (uint16_t*)calloc(fb_pixels, sizeof(uint16_t));
  r->arena_mem           = (uint8_t*)malloc((size_t)k_viewer_arena_bytes);
  if ((r->pages == nullptr) || (r->names == nullptr) || (r->fb == nullptr) ||
      (r->arena_mem == nullptr)) {
    viewer_free(r);
    return k_ra8_err_no_mem;
  }
  *out = r;
  return k_ra8_ok;
}

/**
 * @brief Open @p path, record its size, and reject an empty file.
 * @param[in,out] r    Reader whose file backing to populate.
 * @param[in]     path Filesystem path.
 * @return ra8_err_t; ::k_ra8_err_not_found on open failure or an empty file.
 * @since 0.1.0
 */
static ra8_err_t viewer_open_file(ra8_viewer_reader_t* r, const char* path)
{
  r->file.fp = fopen(path, "rb");
  if (r->file.fp == nullptr) {
    return k_ra8_err_not_found;
  }
  if (fseeko(r->file.fp, 0, SEEK_END) != 0) {
    return k_ra8_err_not_found;
  }
  const off_t end = ftello(r->file.fp);
  if (end <= 0) {
    return k_ra8_err_not_found;
  }
  r->file.size = (uint64_t)end;
  return k_ra8_ok;
}

ra8_err_t ra8_viewer_open(ra8_viewer_reader_t** out, const char* path)
{
  if ((out == nullptr) || (path == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out = nullptr;
  if (!viewer_is_comic_path(path)) {
    /* TODO: dispatch `.epub` -> ra8_epub + ra8_reflow, and vertical-scroll
     * `.webtoon` bundles -> ra8_webtoon + ra8_tileatlas. Comic is done here. */
    return k_ra8_err_not_supported;
  }

  ra8_viewer_reader_t* r  = nullptr;
  ra8_err_t            rc = viewer_alloc(&r);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = viewer_open_file(r, path);
  if (rc != k_ra8_ok) {
    viewer_free(r);
    return rc;
  }
  rc = ra8_comic_open(&r->comic,
                      viewer_read,
                      &r->file,
                      r->file.size,
                      r->pages,
                      (uint32_t)k_viewer_page_cap,
                      r->names,
                      (uint32_t)k_viewer_name_cap);
  if (rc != k_ra8_ok) {
    viewer_free(r);
    return rc;
  }
  *out = r;
  return k_ra8_ok;
}

uint32_t ra8_viewer_page_count(const ra8_viewer_reader_t* r)
{
  if (r == nullptr) {
    return 0U;
  }
  return ra8_comic_page_count(&r->comic);
}

/**
 * @brief Ensure @p r->page_buf holds at least @p need bytes, growing if required.
 * @param[in,out] r    Reader whose scratch buffer to size.
 * @param[in]     need Required capacity in bytes.
 * @return ra8_err_t; ::k_ra8_err_no_mem when the buffer could not be grown.
 * @since 0.1.0
 */
static ra8_err_t viewer_reserve_page_buf(ra8_viewer_reader_t* r, size_t need)
{
  if (r->page_cap >= need) {
    return k_ra8_ok;
  }
  uint8_t* grown = (uint8_t*)realloc(r->page_buf, need);
  if (grown == nullptr) {
    return k_ra8_err_no_mem;
  }
  r->page_buf = grown;
  r->page_cap = need;
  return k_ra8_ok;
}

/**
 * @brief Fit a source image into the framebuffer preserving aspect, centred.
 * @param[in]  src_w Source width, pixels (>= 1).
 * @param[in]  src_h Source height, pixels (>= 1).
 * @param[out] box   Receives {dst_x, dst_y, fit_w, fit_h} in framebuffer pixels.
 * @since 0.1.0
 */
static void viewer_fit_centered(int32_t src_w, int32_t src_h, int32_t box[4])
{
  const int64_t fb_w  = (int64_t)k_ra8_viewer_fb_width;
  const int64_t fb_h  = (int64_t)k_ra8_viewer_fb_height;
  int64_t       fit_w = fb_w;
  int64_t       fit_h = ((int64_t)src_h * fb_w) / (int64_t)src_w;
  if (fit_h > fb_h) {
    fit_h = fb_h;
    fit_w = ((int64_t)src_w * fb_h) / (int64_t)src_h;
  }
  if (fit_w < 1) {
    fit_w = 1;
  }
  if (fit_h < 1) {
    fit_h = 1;
  }
  box[0] = (int32_t)((fb_w - fit_w) / 2);
  box[1] = (int32_t)((fb_h - fit_h) / 2);
  box[2] = (int32_t)fit_w;
  box[3] = (int32_t)fit_h;
}

ra8_err_t ra8_viewer_render_page(ra8_viewer_reader_t* r, uint32_t page)
{
  if (r == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (page >= ra8_comic_page_count(&r->comic)) {
    return k_ra8_err_out_of_range;
  }

  uint64_t  raw_size = 0U;
  ra8_err_t rc = ra8_comic_page_info(&r->comic, page, nullptr, 0U, nullptr, &raw_size, nullptr);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = viewer_reserve_page_buf(r, (size_t)raw_size);
  if (rc != k_ra8_ok) {
    return rc;
  }
  size_t got = 0U;
  rc         = ra8_comic_page_read(&r->comic, page, r->page_buf, r->page_cap, &got);
  if (rc != k_ra8_ok) {
    return rc;
  }

  rc = ra8_gfx_init(r->fb, k_ra8_viewer_fb_width, k_ra8_viewer_fb_height, k_ra8_gfx_format_rgb565);
  if (rc != k_ra8_ok) {
    return rc;
  }
  (void)ra8_gfx_clear((uint32_t)k_viewer_bg);

  int32_t src_w = 0;
  int32_t src_h = 0;
  rc            = ra8_img_probe_size(r->page_buf, got, &src_w, &src_h);
  if (rc != k_ra8_ok) {
    return rc;
  }
  int32_t box[4] = {0, 0, k_ra8_viewer_fb_width, k_ra8_viewer_fb_height};
  viewer_fit_centered(src_w, src_h, box);

  ra8_img_arena_t arena = {.base = r->arena_mem, .cap = (size_t)k_viewer_arena_bytes};
  return ra8_img_decode_blit(&arena,
                             r->page_buf,
                             got,
                             box[0],
                             box[1],
                             box[2],
                             box[3],
                             nullptr,
                             nullptr);
}

const uint16_t* ra8_viewer_framebuffer(const ra8_viewer_reader_t* r)
{
  if (r == nullptr) {
    return nullptr;
  }
  return r->fb;
}

uint16_t ra8_viewer_fb_width(const ra8_viewer_reader_t* r)
{
  return (r == nullptr) ? 0U : (uint16_t)k_ra8_viewer_fb_width;
}

uint16_t ra8_viewer_fb_height(const ra8_viewer_reader_t* r)
{
  return (r == nullptr) ? 0U : (uint16_t)k_ra8_viewer_fb_height;
}

ra8_err_t ra8_viewer_dump_ppm(const ra8_viewer_reader_t* r, const char* path)
{
  if ((r == nullptr) || (path == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  FILE* fp = fopen(path, "wb");
  if (fp == nullptr) {
    return k_ra8_err_not_found;
  }
  (void)fprintf(fp,
                "P6\n%u %u\n255\n",
                (unsigned)k_ra8_viewer_fb_width,
                (unsigned)k_ra8_viewer_fb_height);
  const size_t pixels = (size_t)k_ra8_viewer_fb_width * (size_t)k_ra8_viewer_fb_height;
  for (size_t i = 0U; i < pixels; ++i) {
    const uint16_t p      = r->fb[i];
    const uint32_t r5     = (uint32_t)((p >> 11) & 0x1FU);
    const uint32_t g6     = (uint32_t)((p >> 5) & 0x3FU);
    const uint32_t b5     = (uint32_t)(p & 0x1FU);
    const uint8_t  rgb[3] = {(uint8_t)((r5 << 3) | (r5 >> 2)),
                             (uint8_t)((g6 << 2) | (g6 >> 4)),
                             (uint8_t)((b5 << 3) | (b5 >> 2))};
    (void)fwrite(rgb, 1U, sizeof(rgb), fp);
  }
  const int closed = fclose(fp);
  return (closed == 0) ? k_ra8_ok : k_ra8_err_not_found;
}

void ra8_viewer_close(ra8_viewer_reader_t* r)
{
  viewer_free(r);
}
