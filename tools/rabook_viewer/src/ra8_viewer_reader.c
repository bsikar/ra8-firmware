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
 * @since 0.1.0
 *
 *

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

ra8_err_t viewer_reserve_page_buf(ra8_viewer_reader_t* r, size_t need, ra8_arena_t* arena)
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
  uint8_t* grown = (uint8_t*)ra8_arena_alloc(arena, (uint32_t)need, k_ra8_arena_align);
  if (grown == nullptr) {
    // cppcheck-suppress memleak
    // grown is nullptr here
    return k_ra8_err_no_mem;
  }
  if (r->page_buf != nullptr) {
    memcpy(grown, r->page_buf, r->page_cap);
  }
  r->page_buf = grown;
  r->page_cap = need;
  return k_ra8_ok;
}

/**
 * @brief Free every owned buffer of @p r and the handle itself.
 * @details Closes the comic engine and the file, frees the JOF and comic backing
 *          buffers (all nullptr unless that engine is active, and free(nullptr) is a
 *          no-op), then the framebuffer, index arrays and the handle. Safe on a
 *          partially-constructed reader, which is why open() uses it as its single
 *          cleanup path.
 * @param[in,out] r Reader to release (nullptr is ignored).
 * @pre @p r came from a viewer_alloc_* path, or is nullptr.
 * @pre @p r is not used again after this call.
 * @post Every buffer the reader owns is released exactly once.
 * @post The handle @p r is freed and must not be dereferenced.
 * @note Not thread-safe (frees shared state).
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
  /* JOF buffers (all nullptr unless this is a JOF document; free(nullptr) is ok). */
  /* Arena memory is released by resetting the arena; no free needed */
}

/**
 * @brief Test whether @p path ends (case-insensitively) with @p ext.
 * @details Compares the tail of @p path against @p ext, folding ASCII uppercase
 *          to lowercase so `.CBZ` matches `.cbz`. A path shorter than @p ext can
 *          never match.
 * @param[in] path Path to test (non-nullptr).
 * @param[in] ext  Lowercase extension including the dot (e.g. ".cbz").
 * @return true when @p path ends with @p ext.
 * @retval false @p path is shorter than @p ext, or the tails differ.
 * @pre @p path and @p ext are NUL-terminated.
 * @pre @p ext is already lowercase.
 * @post No state is mutated.
 * @post The result depends only on the arguments.
 * @note Pure; thread-safe.
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
 * @param[in] path Path to classify (non-nullptr).
 * @return The ::viewer_fmt_t for @p path (::k_vfmt_unsupported if unknown).
 * @retval k_vfmt_unsupported No known extension matched @p path.
 * @pre @p path is a NUL-terminated string.
 * @pre @p path carries the document's real extension.
 * @post No state is mutated.
 * @post A compression suffix is classified before the base extension.
 * @note Pure; thread-safe.
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
 * @details Zero-allocates the reader, allocates the fixed RGB565 framebuffer,
 *          points the JOF blit target at it (the headless default), and installs
 *          the owner's decompression policy. On any failure it releases the
 *          partial reader via viewer_free() so nothing leaks.
 * @param[out] out Receives the allocated reader on success (non-nullptr).
 * @return ra8_err_t; ::k_ra8_err_no_mem on any allocation failure.
 * @retval k_ra8_ok         The reader and framebuffer are allocated.
 * @retval k_ra8_err_no_mem The reader or framebuffer could not be allocated.
 * @pre @p out is writable.
 * @pre The process may allocate the framebuffer.
 * @post On ::k_ra8_ok `*out` owns a zeroed reader with a framebuffer.
 * @post On failure nothing is leaked and `*out` is untouched.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t viewer_alloc_core(ra8_viewer_reader_t** out, ra8_arena_t* arena)
{
  ra8_viewer_reader_t* r = (ra8_viewer_reader_t*)ra8_arena_calloc(arena, 1U, sizeof(*r));
  if (r == nullptr) {
    // cppcheck-suppress memleak
    // freed by viewer_free(r)
    return k_ra8_err_no_mem;
  }
  const size_t fb_pixels = (size_t)k_ra8_viewer_fb_width * (size_t)k_ra8_viewer_fb_height;
  r->fb = (uint16_t*)ra8_arena_calloc(arena, (uint32_t)fb_pixels, sizeof(uint16_t));
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
 * @details Allocates the page index, the page-name arena, the image-decode arena,
 *          and the gzip/xz unwrap and xz-scratch buffers. Partial allocations are
 *          left in place for viewer_free() to release, so the error path needs no
 *          cleanup.
 * @param[in,out] r Reader to populate (non-nullptr).
 * @return ra8_err_t; ::k_ra8_err_no_mem on any allocation failure.
 * @retval k_ra8_ok         Every comic scratch buffer was allocated.
 * @retval k_ra8_err_no_mem At least one allocation failed.
 * @pre @p r is an allocated reader (viewer_alloc_core()).
 * @pre The document classified as a comic format.
 * @post On ::k_ra8_ok every comic scratch buffer is allocated.
 * @post Partial allocations are retained for viewer_free() to release.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t viewer_alloc_comic(ra8_viewer_reader_t* r, ra8_arena_t* arena)
{
  r->pages =
    (ra8_comic_page_t*)ra8_arena_calloc(arena, (uint32_t)k_viewer_page_cap, sizeof(*r->pages));
  r->names = (char*)ra8_arena_calloc(arena, (uint32_t)k_viewer_name_cap, sizeof(char));
  r->arena_mem =
    (uint8_t*)ra8_arena_alloc(arena, (uint32_t)k_viewer_arena_bytes, k_ra8_arena_align);
  r->unwrap = (uint8_t*)ra8_arena_alloc(arena, (uint32_t)k_viewer_unwrap_bytes, k_ra8_arena_align);
  r->xz_scratch =
    (uint8_t*)ra8_arena_alloc(arena, (uint32_t)k_viewer_xz_scratch, k_ra8_arena_align);
  if ((r->pages == nullptr) || (r->names == nullptr) || (r->arena_mem == nullptr) ||
      (r->unwrap == nullptr) || (r->xz_scratch == nullptr)) {
    return k_ra8_err_no_mem;
  }
  return k_ra8_ok;
}

/**
 * @brief Open @p path, record its size, and reject an empty file.
 * @details Opens the file for binary reading and measures its length by seeking
 *          to the end; a missing file, an unseekable stream, or a zero-length
 *          file is rejected so later engines can assume a non-empty document.
 * @param[in,out] r    Reader whose file backing to populate (non-nullptr).
 * @param[in]     path Filesystem path (non-nullptr).
 * @return ra8_err_t; ::k_ra8_err_not_found on open failure or an empty file.
 * @retval k_ra8_ok            The file is open and its size recorded.
 * @retval k_ra8_err_not_found The file could not be opened, sized, or is empty.
 * @pre @p r is an allocated reader and @p path is NUL-terminated.
 * @pre @p r->file.fp is not already open.
 * @post On ::k_ra8_ok `r->file.fp` is open and `r->file.size` is non-zero.
 * @post On failure the reader owns no open file for @p path.
 * @note Not thread-safe.
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
 * @param[in] path The offending path (for the message, non-nullptr).
 * @details EPUB and RABOOK are recognised extensions whose reflow render engine
 *          is not yet wired into the viewer; those get a specific "not wired yet"
 *          message, and anything else gets an "unsupported file type" message.
 *          Either way the caller receives ::k_ra8_err_not_supported.
 * @return Always ::k_ra8_err_not_supported.
 * @retval k_ra8_err_not_supported Always; the message distinguishes the cases.
 * @pre @p path is NUL-terminated.
 * @pre @p fmt is a format with no wired engine.
 * @post A diagnostic naming @p path has been written to stderr.
 * @post No reader state is mutated.
 * @note Not thread-safe (writes stderr).
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
 * @details Maps the classified format to its opener -- bare or wrapped comic, or
 *          JOF -- and returns ::k_ra8_err_not_supported for any format with no
 *          wired engine, so the single call site needs no per-format branching.
 * @param[in,out] r   Reader with file backing populated (non-nullptr).
 * @param[in]     fmt The document's format.
 * @return ra8_err_t from the selected engine opener.
 * @retval k_ra8_err_not_supported @p fmt has no wired engine.
 * @pre @p r has its file backing populated (viewer_open_file()).
 * @pre @p fmt came from viewer_classify().
 * @post On success the selected engine holds an open document.
 * @post An unwired @p fmt opens nothing.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
viewer_dispatch_open(ra8_viewer_reader_t* r, viewer_fmt_t fmt, ra8_arena_t* arena)
{
  switch (fmt) {
    case k_vfmt_comic:
      return viewer_open_comic(r, false, arena);
    case k_vfmt_comic_wrap:
      return viewer_open_comic(r, true, arena);
    case k_vfmt_jof:
      return viewer_open_jof(r, arena);
    default:
      return k_ra8_err_not_supported;
  }
}

/**
 * @brief Probe and cache every tile's native size for the scroll layout.
 * @details JOF bands are sized arithmetically from the parsed atlas geometry;
 *          comic pages must each be probed through the image decoder.
 * @param[in,out] r Reader with an open engine (non-nullptr).
 * @return ra8_err_t; ::k_ra8_err_no_mem if the size arrays cannot be allocated.
 * @retval k_ra8_ok         Tile dimensions are cached (or the document is empty).
 * @retval k_ra8_err_no_mem The per-tile size arrays could not be allocated.
 * @pre @p r has an open engine (comic or JOF).
 * @pre @p r->tile_wpx / tile_hpx are not already allocated.
 * @post On ::k_ra8_ok `r->tile_n` tiles have cached dimensions.
 * @post A zero-page document leaves the size arrays unallocated.
 * @note Not thread-safe (writes the shared size cache).
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t viewer_compute_tiles(ra8_viewer_reader_t* r, ra8_arena_t* arena)
{
  const uint32_t n = ra8_viewer_page_count(r);
  r->tile_n        = n;
  if (n == 0U) {
    return k_ra8_ok;
  }
  r->tile_wpx = (uint32_t*)ra8_arena_calloc(arena, n, sizeof(uint32_t));
  r->tile_hpx = (uint32_t*)ra8_arena_calloc(arena, n, sizeof(uint32_t));
  if ((r->tile_wpx == nullptr) || (r->tile_hpx == nullptr)) {
    return k_ra8_err_no_mem;
  }
  if (r->fmt == k_vfmt_jof) {
    viewer_size_jof_tiles(r, n);
    return k_ra8_ok;
  }
  ra8_img_arena_t arena_img = {.base = r->arena_mem, .cap = (size_t)k_viewer_arena_bytes};
  for (uint32_t i = 0U; i < n; ++i) {
    viewer_probe_comic_tile(r, i, &arena_img, arena);
  }
  return k_ra8_ok;
}

ra8_err_t ra8_viewer_open(ra8_viewer_reader_t** out, const char* path, ra8_arena_t* arena)
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
  ra8_err_t            rc = viewer_alloc_core(&r, arena);
  if (rc != k_ra8_ok) {
    return rc;
  }
  r->fmt = fmt;
  if ((fmt == k_vfmt_comic) || (fmt == k_vfmt_comic_wrap)) {
    rc = viewer_alloc_comic(r, arena);
    if (rc != k_ra8_ok) {
      viewer_free(r);
      return rc;
    }
  }
  rc = viewer_open_file(r, path);
  if (rc == k_ra8_ok) {
    rc = viewer_dispatch_open(r, fmt, arena);
  }
  if (rc == k_ra8_ok) {
    rc = viewer_compute_tiles(r, arena);
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

ra8_err_t ra8_viewer_render_page(ra8_viewer_reader_t* r, uint32_t page, ra8_arena_t* arena)
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
  return viewer_render_comic(r, page, arena);
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
                                    uint16_t**           out,
                                    ra8_arena_t*         arena)
{
  if ((r == nullptr) || (w == nullptr) || (h == nullptr) || (out == nullptr)) {
    return k_ra8_err_null_ptr;
  }
  *out = nullptr;
  if (i >= r->tile_n) {
    return k_ra8_err_out_of_range;
  }
  if (r->fmt == k_vfmt_jof) {
    return viewer_tile_jof(r, i, w, h, out, arena);
  }
  return viewer_tile_comic(r, i, w, h, out, arena);
}

void ra8_viewer_close(ra8_viewer_reader_t* r)
{
  viewer_free(r);
}
