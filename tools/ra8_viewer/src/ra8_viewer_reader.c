/**
 * @file ra8_viewer_reader.c
 * @brief Host-side reader core: format dispatch, buffer ownership, public API.
 *
 * @details
 * A document is opened over a `pread`-style seek+read callback (::viewer_read)
 * so the firmware's demand-paged engines stream it exactly as they do off the SD
 * card on the board. This translation unit owns the handle lifecycle -- classify,
 * allocate, open, size the scroll tiles, free -- and routes every render to one
 * of two engines:
 *
 *   - **Comics** (`.cbz` / `.cbr` / `.cbt`, and gzip/xz-wrapped variants) through
 *     ra8_comic, in ra8_viewer_comic.c.
 *   - **JOF long strips** (`.jof`, the firmware-native vertical-scroll tile
 *     atlas the downloader writes) through ra8_longstrip, in ra8_viewer_jof.c.
 *
 * EPUB (`.epub`) and RABOOK (`.rabook`) reflow text/images through ra8_reflow +
 * a registered font face + an image loader; that stack is not wired here yet and
 * their open returns ::k_ra8_err_not_supported with a clear message.
 *
 * Two render surfaces sit on top of the engines: a fixed framebuffer
 * (::ra8_viewer_render_page, used by the headless PPM dump) and a continuous-
 * scroll **tile** API (::ra8_viewer_render_tile565) that rasterises each page or
 * band at native resolution for the desktop window to scale to width. The
 * Objective-C window backend consumes the tiles; the reader stays testable and
 * dumpable headless.
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
#include "ra8_img_arena.h"
#include "ra8_viewer_reader_internal.h"

size_t viewer_read(void* ctx, uint64_t offset, void* buf, size_t len)
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

ra8_err_t viewer_reserve_page_buf(ra8_viewer_reader_t* r, size_t need)
{
  /* Fail closed before touching the allocator: `need` is the encoded-image
   * length declared in the archive header, so an attacker-chosen value must be
   * rejected against the per-unit output cap rather than handed to realloc. */
  if ((uint64_t)need > r->limits.max_output_bytes) {
    return k_ra8_err_decomp_output_cap;
  }
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
 * @brief Free every owned buffer of @p r and the handle itself.
 * @param[in,out] r Reader to release (NULL is ignored).
 * @post Every buffer the reader owns is released exactly once.
 * @since 0.1.0
 */
RA8_INTERNAL static void viewer_free(ra8_viewer_reader_t* r)
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
  /* JOF buffers (all NULL unless this is a JOF document; free(NULL) is ok). */
  free(r->jof.scratch);
  free(r->jof.buckets);
  free(r->jof.dims);
  free(r->jof.keys);
  free(r->jof.meta);
  free(r->jof.cells);
  free(r->jof.atlas);
  /* Comic buffers. */
  free(r->xz_scratch);
  free(r->unwrap);
  free(r->arena_mem);
  free(r->page_buf);
  free(r->tile_wpx);
  free(r->tile_hpx);
  free(r->fb);
  free(r->names);
  free(r->pages);
  free(r);
}

/**
 * @brief Test whether @p path ends (case-insensitively) with @p ext.
 * @param[in] path Path to test (non-NULL).
 * @param[in] ext  Lowercase extension including the dot (e.g. ".cbz").
 * @return true when @p path ends with @p ext.
 * @since 0.1.0
 */
RA8_INTERNAL static bool viewer_ends_with(const char* path, const char* ext)
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
 * @brief Classify @p path by extension into a reader-engine format.
 * @details A compression suffix wins over the base extension, so a `.cbt.gz` is
 *          a wrapped comic rather than an opaque `.gz`.
 * @param[in] path Path to classify (non-NULL).
 * @return The ::viewer_fmt_t for @p path (::k_vfmt_unsupported if unknown).
 * @since 0.1.0
 */
RA8_INTERNAL static viewer_fmt_t viewer_classify(const char* path)
{
  if (viewer_ends_with(path, ".gz") || viewer_ends_with(path, ".xz")) {
    return k_vfmt_comic_wrap;
  }
  if (viewer_ends_with(path, ".cbz") || viewer_ends_with(path, ".cbr") ||
      viewer_ends_with(path, ".cbt")) {
    return k_vfmt_comic;
  }
  if (viewer_ends_with(path, ".epub") || viewer_ends_with(path, ".epb")) {
    return k_vfmt_epub;
  }
  if (viewer_ends_with(path, ".jof")) {
    return k_vfmt_jof;
  }
  if (viewer_ends_with(path, ".rabook") || viewer_ends_with(path, ".rbk")) {
    return k_vfmt_rabook;
  }
  return k_vfmt_unsupported;
}

/**
 * @brief Allocate the reader handle and the always-needed framebuffer.
 * @param[out] out Receives the allocated reader on success (non-NULL).
 * @return ra8_err_t; ::k_ra8_err_no_mem on any allocation failure.
 * @post On failure nothing is leaked and `*out` is untouched.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t viewer_alloc_core(ra8_viewer_reader_t** out)
{
  ra8_viewer_reader_t* r = (ra8_viewer_reader_t*)calloc(1U, sizeof(*r));
  if (r == nullptr) {
    return k_ra8_err_no_mem;
  }
  const size_t fb_pixels = (size_t)k_ra8_viewer_fb_width * (size_t)k_ra8_viewer_fb_height;
  r->fb                  = (uint16_t*)calloc(fb_pixels, sizeof(uint16_t));
  if (r->fb == nullptr) {
    viewer_free(r);
    return k_ra8_err_no_mem;
  }
  /* Default JOF blit target is the fixed framebuffer (headless path); the tile
   * path retargets it per render. */
  r->rt565 = r->fb;
  r->rt_w  = (uint32_t)k_ra8_viewer_fb_width;
  r->rt_h  = (uint32_t)k_ra8_viewer_fb_height;
  /* One owner-approved decompression policy governs every allocation the viewer
   * sizes from untrusted archive fields (page buffers, the JOF atlas slurp, the
   * JOF band cache). It is the same 64 MiB output cap / 1024:1 ratio bound the
   * firmware reader core enforces, so the viewer rejects exactly what the board
   * would. */
  r->limits = ra8_decomp_limits_default();
  *out      = r;
  return k_ra8_ok;
}

/**
 * @brief Allocate the comic engine's large scratch buffers (comic formats only).
 * @param[in,out] r Reader to populate (non-NULL).
 * @return ra8_err_t; ::k_ra8_err_no_mem on any allocation failure.
 * @post Partial allocations are retained for viewer_free() to release.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t viewer_alloc_comic(ra8_viewer_reader_t* r)
{
  r->pages      = (ra8_comic_page_t*)calloc((size_t)k_viewer_page_cap, sizeof(*r->pages));
  r->names      = (char*)calloc((size_t)k_viewer_name_cap, sizeof(char));
  r->arena_mem  = (uint8_t*)malloc((size_t)k_viewer_arena_bytes);
  r->unwrap     = (uint8_t*)malloc((size_t)k_viewer_unwrap_bytes);
  r->xz_scratch = (uint8_t*)malloc((size_t)k_viewer_xz_scratch);
  if ((r->pages == nullptr) || (r->names == nullptr) || (r->arena_mem == nullptr) ||
      (r->unwrap == nullptr) || (r->xz_scratch == nullptr)) {
    return k_ra8_err_no_mem;
  }
  return k_ra8_ok;
}

/**
 * @brief Open @p path, record its size, and reject an empty file.
 * @param[in,out] r    Reader whose file backing to populate (non-NULL).
 * @param[in]     path Filesystem path (non-NULL).
 * @return ra8_err_t; ::k_ra8_err_not_found on open failure or an empty file.
 * @post On ::k_ra8_ok `r->file.fp` is open and `r->file.size` is non-zero.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t viewer_open_file(ra8_viewer_reader_t* r, const char* path)
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

/**
 * @brief Report a recognised-but-unwired or unknown format on stderr.
 * @param[in] fmt  The classified format (::k_vfmt_epub / ::k_vfmt_rabook /
 *                 ::k_vfmt_unsupported).
 * @param[in] path The offending path (for the message, non-NULL).
 * @return Always ::k_ra8_err_not_supported.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t viewer_reject_fmt(viewer_fmt_t fmt, const char* path)
{
  if ((fmt == k_vfmt_epub) || (fmt == k_vfmt_rabook)) {
    /* Recognised, but the epub (ra8_epub) and rabook (ra8_book) reflow render
     * paths -- font face + image loader over ra8_reflow -- are not wired into the
     * viewer yet. A clear message beats a silent error code. */
    (void)fprintf(stderr,
                  "ra8_viewer: '%s' recognised but its reflow reader engine "
                  "(font + image loader) is not wired into the viewer yet "
                  "(comics and JOF long strips render today)\n",
                  path);
    return k_ra8_err_not_supported;
  }
  (void)fprintf(stderr, "ra8_viewer: unsupported file type: %s\n", path);
  return k_ra8_err_not_supported;
}

/**
 * @brief Route an opened reader to its format's engine opener.
 * @param[in,out] r   Reader with file backing populated (non-NULL).
 * @param[in]     fmt The document's format.
 * @return ra8_err_t from the selected engine opener.
 * @retval k_ra8_err_not_supported @p fmt has no wired engine.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t viewer_dispatch_open(ra8_viewer_reader_t* r, viewer_fmt_t fmt)
{
  switch (fmt) {
    case k_vfmt_comic:
      return viewer_open_comic(r, false);
    case k_vfmt_comic_wrap:
      return viewer_open_comic(r, true);
    case k_vfmt_jof:
      return viewer_open_jof(r);
    default:
      return k_ra8_err_not_supported;
  }
}

/**
 * @brief Probe and cache every tile's native size for the scroll layout.
 * @details JOF bands are sized arithmetically from the parsed atlas geometry;
 *          comic pages must each be probed through the image decoder.
 * @param[in,out] r Reader with an open engine (non-NULL).
 * @return ra8_err_t; ::k_ra8_err_no_mem if the size arrays cannot be allocated.
 * @post On ::k_ra8_ok `r->tile_n` tiles have cached dimensions.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t viewer_compute_tiles(ra8_viewer_reader_t* r)
{
  const uint32_t n = ra8_viewer_page_count(r);
  r->tile_n        = n;
  if (n == 0U) {
    return k_ra8_ok;
  }
  r->tile_wpx = (uint32_t*)calloc(n, sizeof(uint32_t));
  r->tile_hpx = (uint32_t*)calloc(n, sizeof(uint32_t));
  if ((r->tile_wpx == nullptr) || (r->tile_hpx == nullptr)) {
    return k_ra8_err_no_mem;
  }
  if (r->fmt == k_vfmt_jof) {
    viewer_size_jof_tiles(r, n);
    return k_ra8_ok;
  }
  ra8_img_arena_t arena = {.base = r->arena_mem, .cap = (size_t)k_viewer_arena_bytes};
  for (uint32_t i = 0U; i < n; ++i) {
    viewer_probe_comic_tile(r, i, &arena);
  }
  return k_ra8_ok;
}

ra8_err_t ra8_viewer_open(ra8_viewer_reader_t** out, const char* path)
{
  if ((out == nullptr) || (path == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out = nullptr;

  const viewer_fmt_t fmt = viewer_classify(path);
  if ((fmt == k_vfmt_epub) || (fmt == k_vfmt_rabook) || (fmt == k_vfmt_unsupported)) {
    return viewer_reject_fmt(fmt, path);
  }

  ra8_viewer_reader_t* r  = nullptr;
  ra8_err_t            rc = viewer_alloc_core(&r);
  if (rc != k_ra8_ok) {
    return rc;
  }
  r->fmt = fmt;
  if ((fmt == k_vfmt_comic) || (fmt == k_vfmt_comic_wrap)) {
    rc = viewer_alloc_comic(r);
    if (rc != k_ra8_ok) {
      viewer_free(r);
      return rc;
    }
  }
  rc = viewer_open_file(r, path);
  if (rc == k_ra8_ok) {
    rc = viewer_dispatch_open(r, fmt);
  }
  if (rc == k_ra8_ok) {
    rc = viewer_compute_tiles(r);
  }
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
  if (r->fmt == k_vfmt_jof) {
    /* Paginate the tall strip into framebuffer-height windows (>= 1 page). */
    const uint32_t h  = r->jof.dctx.info.height;
    const uint32_t vh = r->jof.viewport_h;
    if ((vh == 0U) || (h == 0U)) {
      return 1U;
    }
    return (h + vh - 1U) / vh;
  }
  return ra8_comic_page_count(&r->comic);
}

ra8_err_t ra8_viewer_render_page(ra8_viewer_reader_t* r, uint32_t page)
{
  if (r == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (page >= ra8_viewer_page_count(r)) {
    return k_ra8_err_out_of_range;
  }
  if (r->fmt == k_vfmt_jof) {
    return viewer_render_jof(r, page);
  }
  return viewer_render_comic(r, page);
}

uint32_t ra8_viewer_tile_count(const ra8_viewer_reader_t* r)
{
  return (r == nullptr) ? 0U : r->tile_n;
}

ra8_err_t ra8_viewer_tile_size(const ra8_viewer_reader_t* r, uint32_t i, uint32_t* w, uint32_t* h)
{
  if ((r == nullptr) || (w == nullptr) || (h == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  if (i >= r->tile_n) {
    return k_ra8_err_out_of_range;
  }
  *w = r->tile_wpx[i];
  *h = r->tile_hpx[i];
  return k_ra8_ok;
}

ra8_err_t ra8_viewer_render_tile565(ra8_viewer_reader_t* r,
                                    uint32_t             i,
                                    uint32_t*            w,
                                    uint32_t*            h,
                                    uint16_t**           out)
{
  if ((r == nullptr) || (w == nullptr) || (h == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out = nullptr;
  if (i >= r->tile_n) {
    return k_ra8_err_out_of_range;
  }
  if (r->fmt == k_vfmt_jof) {
    return viewer_tile_jof(r, i, w, h, out);
  }
  return viewer_tile_comic(r, i, w, h, out);
}

void ra8_viewer_close(ra8_viewer_reader_t* r)
{
  viewer_free(r);
}
