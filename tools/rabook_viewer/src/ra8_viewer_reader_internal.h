/**
 * @file ra8_viewer_reader_internal.h
 * @brief Module-private reader state + per-engine entry points for the viewer.
 *
 * @details
 * The host reader core is one logical module split across four translation
 * units: the public API / open dispatch (ra8_viewer_reader.c), the comic engine
 * (ra8_viewer_comic.c), the JOF long-strip engine (ra8_viewer_jof.c), and the
 * shared RGB565 pixel + PPM writer (ra8_viewer_ppm.c). The document handle, its
 * owned buffers, and the fixed sizing knobs are shared by all four, so they live
 * here; nothing in this header is part of the viewer-facing API in
 * ra8_viewer_reader.h.
 *
 * Each engine exposes exactly three ::RA8_PRIV entry points -- open, render into
 * the fixed framebuffer, and render one native-resolution scroll tile -- so the
 * dispatcher in ra8_viewer_reader.c never needs to know an engine's internals.
 *
 * @since 0.1.0
 *
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT

 */

#pragma once

#include <stdint.h>
#include <stdio.h>

#include "ra8_arena.h"
#include "ra8_attributes.h"
#include "ra8_comic.h"
#include "ra8_decomp_limits.h"
#include "ra8_err.h"
#include "ra8_img_arena.h"
#include "ra8_jof.h"
#include "ra8_longstrip.h"
#include "ra8_tile_cache.h"
#include "ra8_unarch_xz.h"
#include "ra8_viewer_reader.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum ra8_viewer_budget_t
 * @brief Fixed capacities for the reader's owned buffers (no magic numbers).
 * @invariant Every value is a byte count except ::k_viewer_page_cap (entries).
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_viewer_page_cap     = 8192U,                /**< Page index; >= fw entry cap.    */
  k_viewer_name_cap     = 512U * 1024U,         /**< Page-name arena, bytes.         */
  k_viewer_arena_bytes  = 160U * 1024U * 1024U, /**< stb_image decode scratch.       */
  k_viewer_unwrap_bytes = 128U * 1024U * 1024U, /**< Unwrap arena; >= fw output cap. */
  k_viewer_xz_scratch   = 4U * 1024U * 1024U,   /**< xz scratch; > fw state reserve. */
} ra8_viewer_budget_t;

/*
 * The capacities the viewer shares with the firmware reader are CHECKED against
 * the firmware policy headers, never silently copied: the viewer over-provisions
 * for a host (it has RAM to spare), but a firmware sizing that grows past that
 * headroom must break THIS build rather than let the viewer quietly preview a
 * document the board would reject (or vice versa). The page index must hold every
 * member the shared archive decoder will enumerate; the gzip/xz unwrap arena must
 * hold the largest container the shared decompression policy admits; the xz
 * scratch must exceed the firmware decoder-state reserve so a dictionary budget
 * remains. (k_viewer_name_cap and k_viewer_arena_bytes are host-local sizing with
 * no firmware counterpart, so they carry no such coupling.)
 */
static_assert((uint64_t)k_viewer_page_cap >= (uint64_t)k_ra8_decomp_def_max_entries,
              "viewer page index must cover the firmware archive entry cap "
              "(ra8_decomp_limits.h)");
static_assert((uint64_t)k_viewer_unwrap_bytes >= (uint64_t)k_ra8_decomp_def_output_bytes,
              "viewer unwrap arena must cover the firmware decode output cap "
              "(ra8_decomp_limits.h)");
static_assert((uint64_t)k_viewer_xz_scratch > (uint64_t)k_ra8_unarch_xz_state_reserve,
              "viewer xz scratch must exceed the firmware xz decoder-state reserve "
              "(ra8_unarch_xz.h)");

/**
 * @enum ra8_viewer_jof_budget_t
 * @brief Fixed sizing knobs for the JOF long-strip tile cache.
 * @invariant ::k_viewer_jof_buckets is a power of two (hash-index mask).
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_viewer_jof_max_cells   = 24U,   /**< Max resident bands (LRU decode-on-miss). */
  k_viewer_jof_buckets     = 64U,   /**< Tile-cache hash buckets.                 */
  k_viewer_jof_scratch_pad = 4096U, /**< Deflate-staging slack over one band.     */
} ra8_viewer_jof_budget_t;

/**
 * @enum viewer_fmt_t
 * @brief Document formats the viewer routes to a reader engine.
 * @see viewer_classify()
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_vfmt_unsupported = 0, /**< Unrecognised extension.               */
  k_vfmt_comic       = 1, /**< Bare CBZ/CBR/CBT (ra8_comic).         */
  k_vfmt_comic_wrap  = 2, /**< gzip/xz-wrapped comic (open_wrapped). */
  k_vfmt_epub        = 3, /**< EPUB (ra8_epub + ra8_reflow).         */
  k_vfmt_jof         = 4, /**< JOF atlas (ra8_longstrip).            */
  k_vfmt_rabook      = 5, /**< RABOOK (ra8_book + ra8_reflow).       */
} viewer_fmt_t;

/**
 * @enum ra8_viewer_color_t
 * @brief Framebuffer colours as packed RGB565 words / 0x00RRGGBB words.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_viewer_bg         = 0x00FFFFFFU, /**< Page background: white (e-ink), gfx word. */
  k_viewer_white_byte = 0xFFU,       /**< memset fill giving 0xFFFF RGB565 (white). */
} ra8_viewer_color_t;

/**
 * @struct viewer_file_ctx_t
 * @brief Backing context for the seek+read callback: an open file and its size.
 * @invariant @ref fp is non-NULL and @ref size is non-zero once a reader is open.
 * @since 0.1.0
 */
typedef struct {
  FILE*    fp;   /**< Open document stream.   */
  uint64_t size; /**< Document length, bytes. */
} viewer_file_ctx_t;

/**
 * @struct viewer_jof_t
 * @brief All ra8_longstrip state + owned buffers for one open JOF strip.
 * @details The strip's atlas bytes are slurped into @ref atlas (the memstore
 *          pread reads from it); the tile cache decodes DEFLATE bands on miss
 *          into @ref cells. The strip is paginated: each viewer "page" is one
 *          @ref viewport_h -tall window scrolled down the canvas.
 * @invariant All pointers are NULL unless the document is ::k_vfmt_jof.
 * @since 0.1.0
 */
typedef struct {
  uint8_t*                   atlas;      /**< Whole `.jof` file (owned).              */
  ra8_jof_memstore_t         store;      /**< Memstore pread over @ref atlas.         */
  ra8_longstrip_decode_ctx_t dctx;       /**< Decode ctx: pread + parsed info + scr.  */
  ra8_tile_cache_t           cache;      /**< Band cache (uses the arrays below).     */
  ra8_longstrip_t            strip;      /**< Opened long-strip document.             */
  uint8_t*                   cells;      /**< `cell_count * band_bytes` tile storage. */
  ra8_keycache_cell_t*       meta;       /**< `cell_count` cache link-metadata.       */
  ra8_tile_key_t*            keys;       /**< `cell_count` cache keys.                */
  ra8_tile_dims_t*           dims;       /**< `cell_count` cache dims.                */
  int32_t*                   buckets;    /**< `k_viewer_jof_buckets` bucket heads.    */
  uint8_t*                   scratch;    /**< DEFLATE staging (one band + pad).       */
  uint32_t                   viewport_h; /**< Page height on the canvas (== fb h).    */
  int32_t                    x_off;      /**< Horizontal centring offset in the fb.   */
} viewer_jof_t;

/**
 * @struct ra8_viewer_reader
 * @brief One open document: file backing, engine state, and owned buffers.
 * @invariant @ref fb always owns `k_ra8_viewer_fb_width * k_ra8_viewer_fb_height`
 *            RGB565 pixels once ::ra8_viewer_open has returned ::k_ra8_ok.
 * @invariant @ref rt565 points at @ref fb except while a scroll tile is rendering.
 * @since 0.1.0
 */
struct ra8_viewer_reader {
  viewer_fmt_t        fmt;    /**< Which engine this document uses.          */
  viewer_file_ctx_t   file;   /**< File backing for the read callback.       */
  ra8_decomp_limits_t limits; /**< Untrusted-input allocation policy (open). */
  uint16_t*           fb;     /**< RGB565 framebuffer (owned).               */
  /* --- continuous-scroll tile model (native-resolution, window path) --- */
  uint32_t* tile_wpx; /**< Cached native width per tile (tile_n).  */
  uint32_t* tile_hpx; /**< Cached native height per tile (tile_n). */
  uint32_t  tile_n;   /**< Number of tiles (== page count).        */
  /* JOF composite target: the long-strip blit writes RGB565 here. Points at
   * @ref fb for the headless framebuffer path, or a native-width tile buffer for
   * the window's scroll tiles -- so the two never interfere. */
  uint16_t* rt565; /**< Current JOF blit target (RGB565). */
  uint32_t  rt_w;  /**< Target width, pixels.             */
  uint32_t  rt_h;  /**< Target height, pixels.            */
  /* --- comic engine (k_vfmt_comic / k_vfmt_comic_wrap) --- */
  ra8_comic_t       comic;      /**< Comic engine (CBZ / CBR / CBT).          */
  ra8_comic_page_t* pages;      /**< Page index (k_viewer_page_cap entries).  */
  char*             names;      /**< Page-name arena.                         */
  uint8_t*          page_buf;   /**< Encoded-image scratch (grows on demand). */
  size_t            page_cap;   /**< Capacity of @ref page_buf, bytes.        */
  uint8_t*          arena_mem;  /**< Backing store for the decode arena.      */
  uint8_t*          unwrap;     /**< gzip/xz unwrap arena (out-lives comic).  */
  uint8_t*          xz_scratch; /**< xz decoder scratch (dict + state).       */
  /* --- long-strip / JOF engine (k_vfmt_jof) --- */
  viewer_jof_t jof; /**< JOF strip state (unused for comics). */
};

/**
 * @brief Seek+read callback over the open document (matches ra8_comic_read_fn).
 * @details Shared by every engine so a document streams through one code path,
 *          exactly as the firmware demand-pages it off the SD card.
 * @param[in]  ctx    A ::viewer_file_ctx_t (non-NULL).
 * @param[in]  offset Absolute byte offset into the document.
 * @param[out] buf    Destination buffer (non-NULL).
 * @param[in]  len    Bytes requested.
 * @return Bytes read; 0 at EOF, on a seek failure, or for a NULL argument.
 * @retval 0 EOF, a seek failure, or a NULL @p ctx / @p buf.
 * @pre @p ctx wraps an open document stream.
 * @pre @p buf has room for @p len bytes.
 * @post At most @p len bytes were copied into @p buf.
 * @post The stream position is left at @p offset + the returned count.
 * @note Not thread-safe: it seeks the shared stream.
 * @since 0.1.0
 */
RA8_PRIV size_t viewer_read(void* ctx, uint64_t offset, void* buf, size_t len);

/**
 * @brief Ensure @p r->page_buf holds at least @p need bytes, growing if required.
 * @details Fails closed before allocating when @p need exceeds the reader's
 *          untrusted-input output cap (`r->limits.max_output_bytes`): @p need is
 *          an archive-declared length, so a lying header must be refused rather
 *          than realloc'd. The declared/compressed-size ratio is validated by the
 *          caller (viewer_read_page_bytes) via `ra8_decomp_check_declared`.
 * @param[in,out] r    Reader whose scratch buffer to size (non-NULL).
 * @param[in]     need Required capacity in bytes (archive-derived; untrusted).
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    The buffer holds at least @p need bytes.
 * @retval k_ra8_err_decomp_output_cap @p need exceeds the per-unit output cap.
 * @retval k_ra8_err_no_mem            The buffer could not be grown.
 * @pre @p r is an open reader.
 * @pre @p need is the archive-declared uncompressed length for the page.
 * @post On ::k_ra8_ok `r->page_cap >= need`.
 * @post On ::k_ra8_err_decomp_output_cap no allocation was attempted.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t viewer_reserve_page_buf(ra8_viewer_reader_t* r, size_t need, ra8_arena_t* arena);

/**
 * @brief Open a bare or gzip/xz-wrapped comic archive via ra8_comic.
 * @details Routes to `ra8_comic_open` or, when @p wrapped, `ra8_comic_open_wrapped`
 *          (which unwraps the outer gzip/xz stream into `r->unwrap` first), then
 *          parses the archive into the reader's page index.
 * @param[in,out] r       Reader with file backing populated (non-NULL).
 * @param[in]     wrapped true to route through `ra8_comic_open_wrapped`.
 * @return ra8_err_t from ra8_comic's open.
 * @retval k_ra8_ok The archive parsed and `r->comic` holds a page index.
 * @pre `r->pages` / `r->names` are allocated (see viewer_alloc_comic()).
 * @pre @p r->file wraps the open document stream.
 * @post On ::k_ra8_ok `r->comic` holds a parsed page index.
 * @post On any error the reader owns no comic state.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t viewer_open_comic(ra8_viewer_reader_t* r, bool wrapped, ra8_arena_t* arena);

/**
 * @brief Render one comic page (extract -> decode -> fit-blit) into the fb.
 * @details Extracts the page's encoded image from the archive, decodes it in the
 *          bound arena, then scales it to fit the framebuffer width and centres
 *          it over a white margin. The heavy buffers are borrowed from the
 *          reader, so no large allocation happens per page.
 * @param[in,out] r    Reader of a comic format (non-NULL).
 * @param[in]     page Page index (`< ra8_viewer_page_count(r)`).
 * @return ra8_err_t from the extract / probe / decode pipeline.
 * @retval k_ra8_ok The page was decoded and blitted into `r->fb`.
 * @pre The document was opened as ::k_vfmt_comic or ::k_vfmt_comic_wrap.
 * @pre @p page is below the document's page count.
 * @post On ::k_ra8_ok `r->fb` holds the page scaled to fit and centred.
 * @post On any error `r->fb` is left as it was.
 * @note Not thread-safe (drives the shared framebuffer and arena).
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t viewer_render_comic(ra8_viewer_reader_t* r, uint32_t page, ra8_arena_t* arena);

/**
 * @brief Render comic page @p i into a fresh RGB565 buffer (gfx-max capped).
 * @details The window's scroll path renders each page at native resolution into
 *          its own buffer (dimensions capped at the graphics maximum) rather than
 *          the fixed framebuffer, so tiles compose independently as they scroll.
 * @param[in,out] r   Reader of a comic format (non-NULL).
 * @param[in]     i   Tile (page) index.
 * @param[out]    w   Receives the rendered width in pixels.
 * @param[out]    h   Receives the rendered height in pixels.
 * @param[out]    out Receives an arena-allocated `w*h` RGB565 buffer.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Tile rendered; `*out` owns `w*h` pixels.
 * @retval k_ra8_err_no_mem        The output buffer could not be allocated.
 * @retval k_ra8_err_not_supported Page @p i failed to probe at open time.
 * @pre The document was opened as a comic format and @p i is a valid page.
 * @pre @p w, @p h and @p out are writable.
 * @post On ::k_ra8_ok `*out` owns a `w*h` RGB565 buffer the caller frees.
 * @post On any error `*out` is left untouched and nothing is leaked.
 * @note Not thread-safe (drives the shared reader and arena).
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t viewer_tile_comic(ra8_viewer_reader_t* r,
                                     uint32_t             i,
                                     uint32_t*            w,
                                     uint32_t*            h,
                                     uint16_t**           out,
                                     ra8_arena_t*         arena);

/**
 * @brief Probe one comic page's native size into the tile-size cache.
 * @details A page that fails to probe stays 0x0 and the view draws a placeholder
 *          for it, so one unreadable page never sinks the whole document.
 * @param[in,out] r     Reader of a comic format (non-NULL).
 * @param[in]     i     Page index.
 * @param[in]     arena Bindable decode arena for the JPEG header probe.
 * @pre @p r was opened as a comic format and @p i is a valid page.
 * @pre @p arena is a decode arena the probe may bind.
 * @post `r->tile_wpx[i]` / `r->tile_hpx[i]` are set, or left 0 on failure.
 * @post No page image is fully decoded (header probe only).
 * @note Not thread-safe (writes the shared size cache).
 * @since 0.1.0
 */
RA8_PRIV void viewer_probe_comic_tile(ra8_viewer_reader_t* r,
                                      uint32_t             i,
                                      ra8_img_arena_t*     img_arena,
                                      ra8_arena_t*         arena);

/**
 * @brief Open a JOF strip: slurp, parse, size the cache, then wire the engine.
 * @details Reads the whole `.jof` into an owned buffer, parses its atlas header,
 *          sizes and initialises the band tile-cache, and opens the long-strip
 *          document over the memstore -- so later renders only decode bands on
 *          demand.
 * @param[in,out] r Reader with file backing populated (non-NULL).
 * @return ra8_err_t from the parse / cache-init / long-strip-open pipeline.
 * @retval k_ra8_ok The strip opened and `r->jof.strip` is ready.
 * @pre The document classified as ::k_vfmt_jof.
 * @pre @p r->file wraps the open document stream.
 * @post On ::k_ra8_ok `r->jof.strip` is an open long-strip document.
 * @post On any error the reader owns no JOF state.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t viewer_open_jof(ra8_viewer_reader_t* r, ra8_arena_t* arena);

/**
 * @brief Render one JOF "page": scroll the strip by one viewport and composite.
 * @details Scrolls the long-strip canvas down by @p page viewports, renders the
 *          visible band (decoding DEFLATE bands on cache miss), and composites it
 *          centred over a white margin into the fixed framebuffer.
 * @param[in,out] r    Reader of a JOF document (non-NULL).
 * @param[in]     page Vertical page index (window number down the strip).
 * @return ra8_err_t from the scroll / long-strip-render pipeline.
 * @retval k_ra8_ok The band was rendered into `r->fb`.
 * @pre The document was opened as ::k_vfmt_jof.
 * @pre @p page is below the document's page count.
 * @post On ::k_ra8_ok `r->fb` holds the band, centred, over a white margin.
 * @post On any error `r->fb` is left as it was.
 * @note Not thread-safe (drives the shared framebuffer and band cache).
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t viewer_render_jof(ra8_viewer_reader_t* r, uint32_t page);

/**
 * @brief Render JOF band @p i at native strip width into a fresh RGB565 buffer.
 * @details Points the strip's blit target at a freshly allocated native-width
 *          buffer, renders band @p i into it, and restores the fixed framebuffer
 *          as the target on every exit -- so the window's scroll tiles never
 *          disturb the headless framebuffer path.
 * @param[in,out] r   Reader of a JOF document (non-NULL).
 * @param[in]     i   Band (tile) index.
 * @param[out]    w   Receives the rendered width in pixels.
 * @param[out]    h   Receives the rendered height in pixels.
 * @param[out]    out Receives an arena-allocated `w*h` RGB565 buffer.
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Band rendered; `*out` owns `w*h` pixels.
 * @retval k_ra8_err_no_mem       The output buffer could not be allocated.
 * @retval k_ra8_err_invalid_size Band @p i has a zero dimension.
 * @pre The document was opened as ::k_vfmt_jof and @p i is a valid band.
 * @pre @p w, @p h and @p out are writable.
 * @post The blit target is restored to the fixed framebuffer on every path.
 * @post On any error `*out` is left untouched and nothing is leaked.
 * @note Not thread-safe (drives the shared reader and band cache).
 * @since 0.1.0
 */
ra8_err_t viewer_tile_jof(ra8_viewer_reader_t* r,
                          uint32_t             i,
                          uint32_t*            w,
                          uint32_t*            h,
                          uint16_t**           out,
                          ra8_arena_t*         arena);

/**
 * @brief Populate the per-band tile-size cache for an open JOF strip.
 * @details Each tile is one viewport-height band; the last is clipped to the
 *          remaining rows so the document has no trailing white gap.
 * @param[in,out] r Reader of a JOF document (non-NULL).
 * @param[in]     n Tile count (`ra8_viewer_page_count(r)`).
 * @pre The document was opened as ::k_vfmt_jof and its strip is open.
 * @pre @p n equals `ra8_viewer_page_count(r)`.
 * @post `r->tile_wpx` / `r->tile_hpx` are filled for all @p n tiles.
 * @post The last band's height is clipped to the strip's remaining rows.
 * @note Not thread-safe (writes the shared size cache).
 * @since 0.1.0
 */
RA8_PRIV void viewer_size_jof_tiles(ra8_viewer_reader_t* r, uint32_t n);

#ifdef __cplusplus
}
#endif
