/**
 * @file ra8_viewer_reader.c
 * @brief Host-side reader core: file-backed reader engines into an RGB565 fb.
 *
 * @details
 * A document is opened over a `pread`-style seek+read callback (::viewer_read)
 * so the firmware's demand-paged engines stream it exactly as they do off the SD
 * card on the board. Two engines are wired:
 *
 *   - **Comics** (`.cbz` / `.cbr` / `.cbt`, and gzip/xz-wrapped variants) drive
 *     ra8_comic. Each page is extracted to an encoded-image scratch buffer and
 *     handed to ra8_reflow's `ra8_img_decode_blit`, which decodes it through
 *     stb_image (backed by a caller bump arena, no heap inside the decode) and
 *     blits it -- scaled to fit and centred -- into the owned RGB565 framebuffer.
 *
 *   - **Webtoon / RTA1** (`.rta1`, the firmware-native vertical-scroll tile
 *     atlas the downloader writes with `ra8_tileatlas_produce`) drives
 *     ra8_webtoon over an ra8_tile_cache. The tall strip is paginated into
 *     framebuffer-height windows; each "page" scrolls the strip by one viewport
 *     and composites the visible bands (DEFLATE-decoded on miss) into the
 *     framebuffer through a viewer-owned blit that converts to RGB565.
 *
 * EPUB (`.epub`) and RABOOK (`.rabook`) reflow text/images through ra8_reflow +
 * a registered font face + an image loader; that stack is not wired here yet and
 * their open returns ::k_ra8_err_not_supported with a clear message.
 *
 * Two render surfaces sit on top of the engines: a fixed 720x1080 framebuffer
 * (::ra8_viewer_render_page, used by the headless PPM dump) and a continuous-
 * scroll **tile** API (::ra8_viewer_render_tile565) that rasterises each page /
 * webtoon band at native resolution for the desktop window to scale to width.
 * The Objective-C window backend consumes the tiles; the reader stays testable
 * and dumpable headless.
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
#include "ra8_img_arena.h"
#include "ra8_reflow_image.h"
#include "ra8_tile_cache.h"
#include "ra8_tileatlas.h"
#include "ra8_webtoon.h"

/**
 * @enum ra8_viewer_budget_t
 * @brief Fixed capacities for the reader's owned buffers (no magic numbers).
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_viewer_page_cap     = 8192U,                /**< Max pages in the sorted index. */
  k_viewer_name_cap     = 512U * 1024U,         /**< Page-name arena, bytes.        */
  k_viewer_arena_bytes  = 160U * 1024U * 1024U, /**< stb_image decode scratch.     */
  k_viewer_unwrap_bytes = 128U * 1024U * 1024U, /**< gzip/xz decompressed container.*/
  k_viewer_xz_scratch   = 4U * 1024U * 1024U,   /**< xz decoder state + <=1MiB dict.*/
} ra8_viewer_budget_t;

/**
 * @enum ra8_viewer_rta1_budget_t
 * @brief Fixed sizing knobs for the RTA1 (webtoon) tile cache.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_viewer_rta1_max_cells   = 24U,   /**< Max resident bands (LRU decode-on-miss). */
  k_viewer_rta1_buckets     = 64U,   /**< Tile-cache hash buckets.                 */
  k_viewer_rta1_scratch_pad = 4096U, /**< Deflate-staging slack over one band.     */
} ra8_viewer_rta1_budget_t;

/**
 * @enum viewer_fmt_t
 * @brief Document formats the viewer routes to a reader engine.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_vfmt_unsupported = 0, /**< Unrecognised extension.               */
  k_vfmt_comic       = 1, /**< Bare CBZ/CBR/CBT (ra8_comic).         */
  k_vfmt_comic_wrap  = 2, /**< gzip/xz-wrapped comic (open_wrapped). */
  k_vfmt_epub        = 3, /**< EPUB (ra8_epub + ra8_reflow).         */
  k_vfmt_rta1        = 4, /**< RTA1 atlas (ra8_webtoon).             */
  k_vfmt_rabook      = 5, /**< RABOOK (ra8_book + ra8_reflow).       */
} viewer_fmt_t;

/**
 * @enum ra8_viewer_color_t
 * @brief Framebuffer colours as packed RGB565 words / 0x00RRGGBB words.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_viewer_bg         = 0x00FFFFFFU, /**< Page background: white (e-ink), gfx word.  */
  k_viewer_white_byte = 0xFFU,       /**< memset fill giving 0xFFFF RGB565 (white).  */
} ra8_viewer_color_t;

/**
 * @enum ra8_viewer_rgb565_t
 * @brief Field shifts / channel reductions for packing 8-bit RGB into RGB565.
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_rgb565_r_shift = 11U, /**< Red field position in the 16-bit word.   */
  k_rgb565_g_shift = 5U,  /**< Green field position in the 16-bit word.  */
  k_rgb565_r5_drop = 3U,  /**< 8->5-bit reduction for red/blue.          */
  k_rgb565_g6_drop = 2U,  /**< 8->6-bit reduction for green.             */
} ra8_viewer_rgb565_t;

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
 * @struct viewer_rta1_t
 * @brief All ra8_webtoon state + owned buffers for one open RTA1 strip.
 * @details The strip's atlas bytes are slurped into @ref atlas (the memstore
 *          pread reads from it); the tile cache decodes DEFLATE bands on miss
 *          into @ref cells. The strip is paginated: each viewer "page" is one
 *          @ref viewport_h -tall window scrolled down the canvas.
 * @since 0.1.0
 */
typedef struct {
  uint8_t*                 atlas;      /**< Whole `.rta1` file (owned).            */
  ra8_tileatlas_memstore_t store;      /**< Memstore pread over @ref atlas.        */
  ra8_webtoon_decode_ctx_t dctx;       /**< Decode ctx: pread + parsed info + scr. */
  ra8_tile_cache_t         cache;      /**< Band cache (uses the arrays below).    */
  ra8_webtoon_t            wt;         /**< Opened webtoon strip.                  */
  uint8_t*                 cells;      /**< `cell_count * band_bytes` tile storage.*/
  ra8_keycache_cell_t*     meta;       /**< `cell_count` cache link-metadata.      */
  ra8_tile_key_t*          keys;       /**< `cell_count` cache keys.               */
  ra8_tile_dims_t*         dims;       /**< `cell_count` cache dims.               */
  int32_t*                 buckets;    /**< `k_viewer_rta1_buckets` bucket heads.  */
  uint8_t*                 scratch;    /**< DEFLATE staging (one band + pad).      */
  uint32_t                 viewport_h; /**< Page height on the canvas (== fb h).   */
  int32_t                  x_off;      /**< Horizontal centring offset in the fb.  */
} viewer_rta1_t;

/**
 * @struct ra8_viewer_reader
 * @brief One open document: file backing, engine state, and owned buffers.
 * @since 0.1.0
 */
struct ra8_viewer_reader {
  viewer_fmt_t      fmt;  /**< Which engine this document uses.       */
  viewer_file_ctx_t file; /**< File backing for the read callback.    */
  uint16_t*         fb;   /**< RGB565 framebuffer (owned).            */
  /* --- continuous-scroll tile model (native-resolution, window path) --- */
  uint32_t* tile_wpx; /**< Cached native width per tile (tile_n).   */
  uint32_t* tile_hpx; /**< Cached native height per tile (tile_n).  */
  uint32_t  tile_n;   /**< Number of tiles (== page count).         */
  /* RTA1 composite target: the webtoon blit writes RGB565 here. Points at @ref
   * fb for the headless framebuffer path, or a native-width tile buffer for the
   * window's scroll tiles -- so the two never interfere. */
  uint16_t* rt565; /**< Current RTA1 blit target (RGB565).       */
  uint32_t  rt_w;  /**< Target width, pixels.                    */
  uint32_t  rt_h;  /**< Target height, pixels.                   */
  /* --- comic engine (k_vfmt_comic / k_vfmt_comic_wrap) --- */
  ra8_comic_t       comic;      /**< Comic engine (CBZ / CBR / CBT).        */
  ra8_comic_page_t* pages;      /**< Page index (k_viewer_page_cap entries).*/
  char*             names;      /**< Page-name arena.                       */
  uint8_t*          page_buf;   /**< Encoded-image scratch (grows on demand).*/
  size_t            page_cap;   /**< Capacity of @ref page_buf, bytes.      */
  uint8_t*          arena_mem;  /**< Backing store for the decode arena.    */
  uint8_t*          unwrap;     /**< gzip/xz unwrap arena (out-lives comic). */
  uint8_t*          xz_scratch; /**< xz decoder scratch (dict + state).     */
  /* --- webtoon / RTA1 engine (k_vfmt_rta1) --- */
  viewer_rta1_t rta1; /**< RTA1 strip state (unused for comics).  */
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
  /* RTA1 buffers (all NULL unless this is an RTA1 document; free(NULL) is ok). */
  free(r->rta1.scratch);
  free(r->rta1.buckets);
  free(r->rta1.dims);
  free(r->rta1.keys);
  free(r->rta1.meta);
  free(r->rta1.cells);
  free(r->rta1.atlas);
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
 * @brief Classify @p path by extension into a reader-engine format.
 * @param[in] path Path to classify.
 * @return the ::viewer_fmt_t for @p path (::k_vfmt_unsupported if unknown).
 * @since 0.1.0
 */
static viewer_fmt_t viewer_classify(const char* path)
{
  /* Compression suffix wins: a `.cbt.gz` is a wrapped comic, not a `.gz`. */
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
  if (viewer_ends_with(path, ".rta1")) {
    return k_vfmt_rta1;
  }
  if (viewer_ends_with(path, ".rabook") || viewer_ends_with(path, ".rbk")) {
    return k_vfmt_rabook;
  }
  return k_vfmt_unsupported;
}

/**
 * @brief Allocate the reader handle and the always-needed framebuffer.
 * @param[out] out Receives the allocated reader on success.
 * @return ra8_err_t; ::k_ra8_err_no_mem on any allocation failure.
 * @since 0.1.0
 */
static ra8_err_t viewer_alloc_core(ra8_viewer_reader_t** out)
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
  /* Default RTA1 blit target is the fixed framebuffer (headless path); the tile
   * path retargets it per render. */
  r->rt565 = r->fb;
  r->rt_w  = (uint32_t)k_ra8_viewer_fb_width;
  r->rt_h  = (uint32_t)k_ra8_viewer_fb_height;
  *out     = r;
  return k_ra8_ok;
}

/**
 * @brief Allocate the comic engine's large scratch buffers (comic formats only).
 * @param[in,out] r Reader to populate.
 * @return ra8_err_t; ::k_ra8_err_no_mem on any allocation failure.
 * @since 0.1.0
 */
static ra8_err_t viewer_alloc_comic(ra8_viewer_reader_t* r)
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

/** @brief Open a bare CBZ/CBR/CBT via ra8_comic. */
static ra8_err_t viewer_open_comic(ra8_viewer_reader_t* r)
{
  return ra8_comic_open(&r->comic,
                        viewer_read,
                        &r->file,
                        r->file.size,
                        r->pages,
                        (uint32_t)k_viewer_page_cap,
                        r->names,
                        (uint32_t)k_viewer_name_cap);
}

/** @brief Open a gzip/xz-wrapped comic (`.cbt.gz`/`.cbt.xz`) via ra8_comic. */
static ra8_err_t viewer_open_comic_wrapped(ra8_viewer_reader_t* r)
{
  return ra8_comic_open_wrapped(&r->comic,
                                viewer_read,
                                &r->file,
                                r->file.size,
                                r->pages,
                                (uint32_t)k_viewer_page_cap,
                                r->names,
                                (uint32_t)k_viewer_name_cap,
                                r->unwrap,
                                (size_t)k_viewer_unwrap_bytes,
                                r->xz_scratch,
                                (uint32_t)k_viewer_xz_scratch);
}

/**
 * @brief Pack 8-bit RGB into a little-endian RGB565 word.
 * @param[in] rr Red channel (0-255).
 * @param[in] gg Green channel (0-255).
 * @param[in] bb Blue channel (0-255).
 * @return The packed RGB565 value.
 * @since 0.1.0
 */
static inline uint16_t viewer_pack565(uint8_t rr, uint8_t gg, uint8_t bb)
{
  const uint16_t r5 = (uint16_t)(rr >> (uint16_t)k_rgb565_r5_drop);
  const uint16_t g6 = (uint16_t)(gg >> (uint16_t)k_rgb565_g6_drop);
  const uint16_t b5 = (uint16_t)(bb >> (uint16_t)k_rgb565_r5_drop);
  return (uint16_t)((r5 << (uint16_t)k_rgb565_r_shift) | (g6 << (uint16_t)k_rgb565_g_shift) | b5);
}

/**
 * @brief ra8_webtoon composite sink: convert a band sub-window to RGB565 in fb.
 * @details Bands arrive in the strip's native `bpp` (1=gray, 2=RGB565, 3=RGB,
 *          4=RGBA). Coordinates are viewport-relative; the whole viewport is
 *          centred horizontally in the framebuffer via `rta1.x_off`. Pixels
 *          outside the framebuffer are clipped (a partial last page keeps its
 *          background margin).
 * @param[in] ctx   The ::ra8_viewer_reader_t (for `fb` and `rta1.x_off`).
 * @param[in] px    Band pixels, row-major, `bpp` bytes/pixel.
 * @param[in] sw    Sub-window width, pixels.
 * @param[in] sh    Sub-window height, pixels.
 * @param[in] bpp   Bytes per pixel.
 * @param[in] dst_x Viewport-relative destination x of the sub-window.
 * @param[in] dst_y Viewport-relative destination y of the sub-window.
 * @return ::k_ra8_ok always (out-of-bounds pixels are dropped, not an error).
 * @since 0.1.0
 */
static ra8_err_t viewer_rta1_blit(void*          ctx,
                                  const uint8_t* px,
                                  uint16_t       sw,
                                  uint16_t       sh,
                                  uint8_t        bpp,
                                  int32_t        dst_x,
                                  int32_t        dst_y)
{
  ra8_viewer_reader_t* r     = (ra8_viewer_reader_t*)ctx;
  const int32_t        x_off = r->rta1.x_off;
  const int32_t        tw    = (int32_t)r->rt_w;
  const int32_t        th    = (int32_t)r->rt_h;
  for (int32_t row = 0; row < (int32_t)sh; ++row) {
    const int32_t fy = dst_y + row;
    if ((fy < 0) || (fy >= th)) {
      continue;
    }
    for (int32_t col = 0; col < (int32_t)sw; ++col) {
      const int32_t fx = dst_x + col + x_off;
      if ((fx < 0) || (fx >= tw)) {
        continue;
      }
      const uint8_t* sp = &px[(((size_t)row * (size_t)sw) + (size_t)col) * (size_t)bpp];
      uint16_t       pix;
      if (bpp == 1U) {
        pix = viewer_pack565(sp[0], sp[0], sp[0]);
      } else if (bpp == 2U) {
        pix = (uint16_t)((uint16_t)sp[0] | ((uint16_t)sp[1] << 8)); /* already RGB565 LE */
      } else {
        pix = viewer_pack565(sp[0], sp[1], sp[2]); /* bpp 3 (RGB) or 4 (RGBA, drop A) */
      }
      r->rt565[((size_t)fy * (size_t)tw) + (size_t)fx] = pix;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Slurp the open RTA1 file into @p r->rta1.atlas and set up the memstore.
 * @param[in,out] r Reader whose file is already open.
 * @return ra8_err_t; ::k_ra8_err_no_mem / ::k_ra8_err_not_found on failure.
 * @since 0.1.0
 */
static ra8_err_t viewer_rta1_slurp(ra8_viewer_reader_t* r)
{
  const size_t sz  = (size_t)r->file.size;
  uint8_t*     buf = (uint8_t*)malloc(sz);
  if (buf == nullptr) {
    return k_ra8_err_no_mem;
  }
  if (fseeko(r->file.fp, 0, SEEK_SET) != 0) {
    free(buf);
    return k_ra8_err_not_found;
  }
  if (fread(buf, 1U, sz, r->file.fp) != sz) {
    free(buf);
    return k_ra8_err_not_found;
  }
  r->rta1.atlas = buf;
  r->rta1.store = (ra8_tileatlas_memstore_t){.buf = buf, .cap = sz, .len = sz};
  return k_ra8_ok;
}

/**
 * @brief Allocate the RTA1 tile-cache backing arrays for @p cell_count bands.
 * @param[in,out] w          RTA1 state to populate.
 * @param[in]     cell_count Resident-band count.
 * @param[in]     band_bytes Bytes per decoded band.
 * @return ra8_err_t; ::k_ra8_err_no_mem on any allocation failure.
 * @since 0.1.0
 */
static ra8_err_t viewer_rta1_alloc_cache(viewer_rta1_t* w, uint32_t cell_count, size_t band_bytes)
{
  w->scratch = (uint8_t*)malloc(band_bytes + (size_t)k_viewer_rta1_scratch_pad);
  w->cells   = (uint8_t*)malloc((size_t)cell_count * band_bytes);
  w->meta    = (ra8_keycache_cell_t*)calloc(cell_count, sizeof(*w->meta));
  w->keys    = (ra8_tile_key_t*)calloc(cell_count, sizeof(*w->keys));
  w->dims    = (ra8_tile_dims_t*)calloc(cell_count, sizeof(*w->dims));
  w->buckets = (int32_t*)calloc((size_t)k_viewer_rta1_buckets, sizeof(*w->buckets));
  if ((w->scratch == nullptr) || (w->cells == nullptr) || (w->meta == nullptr) ||
      (w->keys == nullptr) || (w->dims == nullptr) || (w->buckets == nullptr)) {
    return k_ra8_err_no_mem;
  }
  return k_ra8_ok;
}

/**
 * @brief Wire the tile cache over the parsed atlas and open the webtoon engine.
 * @param[in,out] r          Reader whose RTA1 buffers are already allocated.
 * @param[in]     info       Parsed atlas geometry.
 * @param[in]     band_bytes Bytes per decoded band.
 * @param[in]     cell_count Resident-band count for the LRU cache.
 * @return ra8_err_t from the tile-cache init / webtoon-open steps.
 * @since 0.1.0
 */
static ra8_err_t viewer_rta1_wire(ra8_viewer_reader_t*        r,
                                  const ra8_tileatlas_info_t* info,
                                  size_t                      band_bytes,
                                  uint32_t                    cell_count)
{
  viewer_rta1_t* w    = &r->rta1;
  w->dctx.scratch     = w->scratch;
  w->dctx.scratch_cap = (uint32_t)(band_bytes + (size_t)k_viewer_rta1_scratch_pad);

  const ra8_tile_cache_cfg_t ccfg = {.cell_mem     = w->cells,
                                     .cell_bytes   = (uint32_t)band_bytes,
                                     .cell_count   = cell_count,
                                     .meta         = w->meta,
                                     .keys         = w->keys,
                                     .dims         = w->dims,
                                     .buckets      = w->buckets,
                                     .bucket_count = (uint32_t)k_viewer_rta1_buckets,
                                     .decode       = ra8_webtoon_tile_decode,
                                     .decode_ctx   = &w->dctx};
  const ra8_err_t            rc   = ra8_tile_cache_init(&w->cache, &ccfg);
  if (rc != k_ra8_ok) {
    return rc;
  }

  /* Compose at the strip's native width; the desktop view scales each tile to
   * the window width, and the headless framebuffer path recentres/clips into its
   * fixed 720px width via `x_off` set per render. */
  const uint16_t viewport_w = info->width;
  w->viewport_h             = (uint32_t)k_ra8_viewer_fb_height;
  w->x_off                  = 0;

  const ra8_webtoon_cfg_t wcfg = {.pread      = ra8_tileatlas_memstore_pread,
                                  .pread_ctx  = &w->store,
                                  .atlas_size = r->file.size,
                                  .cache      = &w->cache,
                                  .image_id   = 1U,
                                  .viewport_w = viewport_w,
                                  .viewport_h = (uint16_t)k_ra8_viewer_fb_height,
                                  .blit       = viewer_rta1_blit,
                                  .blit_ctx   = r};
  return ra8_webtoon_open(&w->wt, &wcfg);
}

/**
 * @brief Open an RTA1 webtoon strip: slurp, parse, size, then wire the engine.
 * @param[in,out] r Reader whose file backing is populated.
 * @return ra8_err_t from the parse / cache-init / webtoon-open pipeline.
 * @since 0.1.0
 */
static ra8_err_t viewer_open_rta1(ra8_viewer_reader_t* r)
{
  viewer_rta1_t* w  = &r->rta1;
  ra8_err_t      rc = viewer_rta1_slurp(r);
  if (rc != k_ra8_ok) {
    return rc;
  }

  w->dctx.pread     = ra8_tileatlas_memstore_pread;
  w->dctx.pread_ctx = &w->store;
  rc = ra8_tileatlas_parse(ra8_tileatlas_memstore_pread, &w->store, r->file.size, &w->dctx.info);
  if (rc != k_ra8_ok) {
    return rc;
  }
  const ra8_tileatlas_info_t info = w->dctx.info;
  const size_t band_bytes         = (size_t)info.tile_w * (size_t)info.tile_h * (size_t)info.bpp;
  if (band_bytes == 0U) {
    return k_ra8_err_invalid_size;
  }
  uint32_t cell_count = info.tile_rows;
  if (cell_count > (uint32_t)k_viewer_rta1_max_cells) {
    cell_count = (uint32_t)k_viewer_rta1_max_cells;
  }
  if (cell_count == 0U) {
    cell_count = 1U;
  }
  rc = viewer_rta1_alloc_cache(w, cell_count, band_bytes);
  if (rc != k_ra8_ok) {
    return rc;
  }
  return viewer_rta1_wire(r, &info, band_bytes, cell_count);
}

/**
 * @brief Report a recognised-but-unwired or unknown format on stderr.
 * @param[in] fmt  The classified format (::k_vfmt_epub / ::k_vfmt_rabook /
 *                 ::k_vfmt_unsupported).
 * @param[in] path The offending path (for the message).
 * @return Always ::k_ra8_err_not_supported.
 * @since 0.1.0
 */
static ra8_err_t viewer_reject_fmt(viewer_fmt_t fmt, const char* path)
{
  if ((fmt == k_vfmt_epub) || (fmt == k_vfmt_rabook)) {
    /* Recognised, but the epub (ra8_epub) and rabook (ra8_book) reflow render
     * paths -- font face + image loader over ra8_reflow -- are not wired into the
     * viewer yet. A clear message beats a silent error code. */
    (void)fprintf(stderr,
                  "ra8_viewer: '%s' recognised but its reflow reader engine "
                  "(font + image loader) is not wired into the viewer yet "
                  "(comics and RTA1 webtoons render today)\n",
                  path);
    return k_ra8_err_not_supported;
  }
  (void)fprintf(stderr, "ra8_viewer: unsupported file type: %s\n", path);
  return k_ra8_err_not_supported;
}

/**
 * @brief Route an opened reader to its format's engine opener.
 * @param[in,out] r   Reader with file backing populated.
 * @param[in]     fmt The document's format.
 * @return ra8_err_t from the selected engine opener.
 * @since 0.1.0
 */
static ra8_err_t viewer_dispatch_open(ra8_viewer_reader_t* r, viewer_fmt_t fmt)
{
  switch (fmt) {
    case k_vfmt_comic:
      return viewer_open_comic(r);
    case k_vfmt_comic_wrap:
      return viewer_open_comic_wrapped(r);
    case k_vfmt_rta1:
      return viewer_open_rta1(r);
    default:
      return k_ra8_err_not_supported;
  }
}

/** @brief Probe + cache every tile's native size for the scroll layout. */
static ra8_err_t viewer_compute_tiles(ra8_viewer_reader_t* r);

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
  if (r->fmt == k_vfmt_rta1) {
    /* Paginate the tall strip into framebuffer-height windows (>= 1 page). */
    const uint32_t h  = r->rta1.dctx.info.height;
    const uint32_t vh = r->rta1.viewport_h;
    if ((vh == 0U) || (h == 0U)) {
      return 1U;
    }
    return (h + vh - 1U) / vh;
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

/**
 * @brief Render one comic page (extract -> decode -> fit-blit) into the fb.
 * @param[in,out] r    Reader (comic format).
 * @param[in]     page Page index.
 * @return ra8_err_t from the extract / decode pipeline.
 * @since 0.1.0
 */
static ra8_err_t viewer_render_comic(ra8_viewer_reader_t* r, uint32_t page)
{
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

  /* stb's allocator is routed through ra8_img_arena; a JPEG header probe needs a
   * bound arena to allocate its decode context (PNG does not), so bind around the
   * probe. ra8_img_decode_blit binds its own arena below. */
  int32_t         src_w       = 0;
  int32_t         src_h       = 0;
  ra8_img_arena_t probe_arena = {.base = r->arena_mem, .cap = (size_t)k_viewer_arena_bytes};
  ra8_img_arena_bind(&probe_arena);
  rc = ra8_img_probe_size(r->page_buf, got, &src_w, &src_h);
  ra8_img_arena_unbind();
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

/**
 * @brief Render one RTA1 "page": scroll the strip by one viewport and composite.
 * @param[in,out] r    Reader (RTA1 format).
 * @param[in]     page Vertical page index (window number down the strip).
 * @return ra8_err_t from the scroll / webtoon-render pipeline.
 * @since 0.1.0
 */
static ra8_err_t viewer_render_rta1(ra8_viewer_reader_t* r, uint32_t page)
{
  viewer_rta1_t* w = &r->rta1;
  /* Headless path: compose into the fixed 720x1080 framebuffer, recentring the
   * native-width strip and clipping any overhang. */
  r->rt565         = r->fb;
  r->rt_w          = (uint32_t)k_ra8_viewer_fb_width;
  r->rt_h          = (uint32_t)k_ra8_viewer_fb_height;
  const int32_t vw = (int32_t)w->dctx.info.width;
  w->x_off         = ((int32_t)r->rt_w > vw) ? (((int32_t)r->rt_w - vw) / 2) : 0;
  /* Clear to white first: a partial last page keeps a paper-white bottom margin.
   * 0xFF bytes == 0xFFFF RGB565 == white. */
  const size_t fb_pixels = (size_t)r->rt_w * (size_t)r->rt_h;
  memset(r->fb, (int)k_viewer_white_byte, fb_pixels * sizeof(*r->fb));

  const int32_t target = (int32_t)page * (int32_t)w->viewport_h;
  const int32_t delta  = target - w->wt.scroll_y;
  ra8_err_t     rc     = ra8_webtoon_scroll_by(&w->wt, delta);
  if (rc != k_ra8_ok) {
    return rc;
  }
  ra8_webtoon_render_stats_t st = {};
  return ra8_webtoon_render(&w->wt, &st);
}

ra8_err_t ra8_viewer_render_page(ra8_viewer_reader_t* r, uint32_t page)
{
  if (r == nullptr) {
    return k_ra8_err_null_ptr;
  }
  if (page >= ra8_viewer_page_count(r)) {
    return k_ra8_err_out_of_range;
  }
  if (r->fmt == k_vfmt_rta1) {
    return viewer_render_rta1(r, page);
  }
  return viewer_render_comic(r, page);
}

/**
 * @brief Probe one comic page's native size into the tile cache (0x0 on failure).
 * @param[in,out] r     Reader (comic format).
 * @param[in]     i     Page index.
 * @param[in]     arena Bound-able decode arena for the JPEG header probe.
 * @since 0.1.0
 */
static void viewer_probe_comic_tile(ra8_viewer_reader_t* r, uint32_t i, ra8_img_arena_t* arena)
{
  uint64_t raw = 0U;
  if (ra8_comic_page_info(&r->comic, i, nullptr, 0U, nullptr, &raw, nullptr) != k_ra8_ok) {
    return;
  }
  if (viewer_reserve_page_buf(r, (size_t)raw) != k_ra8_ok) {
    return;
  }
  size_t got = 0U;
  if (ra8_comic_page_read(&r->comic, i, r->page_buf, r->page_cap, &got) != k_ra8_ok) {
    return;
  }
  int32_t pw = 0;
  int32_t ph = 0;
  ra8_img_arena_bind(arena);
  const ra8_err_t prc = ra8_img_probe_size(r->page_buf, got, &pw, &ph);
  ra8_img_arena_unbind();
  if ((prc == k_ra8_ok) && (pw > 0) && (ph > 0)) {
    r->tile_wpx[i] = (uint32_t)pw;
    r->tile_hpx[i] = (uint32_t)ph;
  }
}

static ra8_err_t viewer_compute_tiles(ra8_viewer_reader_t* r)
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
  if (r->fmt == k_vfmt_rta1) {
    /* Each tile is one viewport-height band of the strip (last one is clipped to
     * the remaining rows so the document has no trailing white gap). */
    const uint32_t vw = r->rta1.dctx.info.width;
    const uint32_t vh = r->rta1.viewport_h;
    const uint32_t hh = r->rta1.dctx.info.height;
    for (uint32_t i = 0U; i < n; ++i) {
      const uint32_t y0 = i * vh;
      uint32_t       bh = (hh > y0) ? (hh - y0) : 0U;
      if (bh > vh) {
        bh = vh;
      }
      r->tile_wpx[i] = vw;
      r->tile_hpx[i] = bh;
    }
    return k_ra8_ok;
  }
  /* Comic: probe every page. A page that fails (e.g. taller than the whole-image
   * decoder's cap) stays 0x0 and the view renders a placeholder for it -- one bad
   * page never sinks the whole document. */
  ra8_img_arena_t arena = {.base = r->arena_mem, .cap = (size_t)k_viewer_arena_bytes};
  for (uint32_t i = 0U; i < n; ++i) {
    viewer_probe_comic_tile(r, i, &arena);
  }
  return k_ra8_ok;
}

/**
 * @brief Fit @p nw x @p nh into the gfx max-edge box, preserving aspect.
 * @details ra8_gfx surfaces cap each edge at ::k_ra8_gfx_max_dim, so a tall page
 *          (a manga spread or a long strip) must be rasterised at a reduced size;
 *          since the view scales every tile to the window width anyway, the only
 *          cost is resolution on very tall pages, not aspect.
 */
static void viewer_cap_render(uint32_t nw, uint32_t nh, uint32_t* rw, uint32_t* rh)
{
  const uint32_t k = (uint32_t)k_ra8_gfx_max_dim;
  uint32_t       w = nw;
  uint32_t       h = nh;
  if (w > k) {
    h = (uint32_t)(((uint64_t)h * k) / w);
    w = k;
  }
  if (h > k) {
    w = (uint32_t)(((uint64_t)w * k) / h);
    h = k;
  }
  *rw = (w < 1U) ? 1U : w;
  *rh = (h < 1U) ? 1U : h;
}

/** @brief Render comic page @p i into a fresh RGB565 buffer (gfx-max capped). */
static ra8_err_t
viewer_tile_comic(ra8_viewer_reader_t* r, uint32_t i, uint32_t* w, uint32_t* h, uint16_t** out)
{
  const uint32_t nw = r->tile_wpx[i];
  const uint32_t nh = r->tile_hpx[i];
  if ((nw == 0U) || (nh == 0U)) {
    return k_ra8_err_not_supported; /* page that failed to probe at open */
  }
  uint32_t rw = 0U;
  uint32_t rh = 0U;
  viewer_cap_render(nw, nh, &rw, &rh);

  uint64_t  raw = 0U;
  ra8_err_t rc  = ra8_comic_page_info(&r->comic, i, nullptr, 0U, nullptr, &raw, nullptr);
  if (rc != k_ra8_ok) {
    return rc;
  }
  rc = viewer_reserve_page_buf(r, (size_t)raw);
  if (rc != k_ra8_ok) {
    return rc;
  }
  size_t got = 0U;
  rc         = ra8_comic_page_read(&r->comic, i, r->page_buf, r->page_cap, &got);
  if (rc != k_ra8_ok) {
    return rc;
  }
  uint16_t* buf = (uint16_t*)malloc((size_t)rw * (size_t)rh * sizeof(uint16_t));
  if (buf == nullptr) {
    return k_ra8_err_no_mem;
  }
  rc = ra8_gfx_init(buf, (uint16_t)rw, (uint16_t)rh, k_ra8_gfx_format_rgb565);
  if (rc == k_ra8_ok) {
    (void)ra8_gfx_clear((uint32_t)k_viewer_bg);
    ra8_img_arena_t arena = {.base = r->arena_mem, .cap = (size_t)k_viewer_arena_bytes};
    rc                    = ra8_img_decode_blit(&arena,
                                                r->page_buf,
                                                got,
                                                0,
                                                0,
                                                (int32_t)rw,
                                                (int32_t)rh,
                                                nullptr,
                                                nullptr);
  }
  if (rc != k_ra8_ok) {
    free(buf);
    return rc;
  }
  *w   = rw;
  *h   = rh;
  *out = buf;
  return k_ra8_ok;
}

/** @brief Render RTA1 band @p i at native strip width into a fresh RGB565 buffer. */
static ra8_err_t
viewer_tile_rta1(ra8_viewer_reader_t* r, uint32_t i, uint32_t* w, uint32_t* h, uint16_t** out)
{
  viewer_rta1_t* wt = &r->rta1;
  const uint32_t nw = r->tile_wpx[i];
  const uint32_t nh = r->tile_hpx[i];
  if ((nw == 0U) || (nh == 0U)) {
    return k_ra8_err_invalid_size;
  }
  const size_t px  = (size_t)nw * (size_t)nh;
  uint16_t*    buf = (uint16_t*)malloc(px * sizeof(uint16_t));
  if (buf == nullptr) {
    return k_ra8_err_no_mem;
  }
  /* Point the webtoon blit at this native-width band buffer (restored after). */
  r->rt565  = buf;
  r->rt_w   = nw;
  r->rt_h   = nh;
  wt->x_off = 0;
  memset(buf, (int)k_viewer_white_byte, px * sizeof(uint16_t));
  const int32_t target = (int32_t)i * (int32_t)wt->viewport_h;
  const int32_t delta  = target - wt->wt.scroll_y;
  ra8_err_t     rc     = ra8_webtoon_scroll_by(&wt->wt, delta);
  if (rc == k_ra8_ok) {
    ra8_webtoon_render_stats_t st = {};
    rc                            = ra8_webtoon_render(&wt->wt, &st);
  }
  r->rt565 = r->fb; /* restore the headless default target */
  r->rt_w  = (uint32_t)k_ra8_viewer_fb_width;
  r->rt_h  = (uint32_t)k_ra8_viewer_fb_height;
  if (rc != k_ra8_ok) {
    free(buf);
    return rc;
  }
  *w   = nw;
  *h   = nh;
  *out = buf;
  return k_ra8_ok;
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
  if (r->fmt == k_vfmt_rta1) {
    return viewer_tile_rta1(r, i, w, h, out);
  }
  return viewer_tile_comic(r, i, w, h, out);
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
