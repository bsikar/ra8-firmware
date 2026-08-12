/**
 * @file ra8_comic_tiles.h
 * @brief Tile an oversized comic page through the JOF atlas + ra8_tile_cache so
 *        a CBZ/CBR page larger than the whole-decode arena opens and zooms (#344).
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * `ra8_comic` streams a page's *encoded* bytes out of a CBZ/CBR archive and the
 * reader hands them to `ra8_img_decode_blit`, which decodes the **whole image**
 * with stb_image into a bump arena. A page whose *decoded* size exceeds that
 * arena cannot be opened at all, and there is no sub-rect access, so the loupe
 * cannot magnify a comic page. That whole-page cap is exactly the case
 * `ra8_jof` was built for and is already wired to the EPUB path
 * (`ra8_epub_tile_binder_import`), but not to comics -- #344.
 *
 * This module is the comic analogue of that EPUB binder, reusing the same
 * tiling infrastructure rather than reinventing it: it does not implement a new
 * decoder, atlas, or cache. On open it transcodes one page's encoded bytes
 * through `ra8_jof_produce()` into a caller-supplied atlas store (an SDRAM
 * memstore), then pages the produced full-resolution tiles through
 * `ra8_tile_cache` -- decode-on-demand, one bounded read plus one bounded
 * inflate per tile miss (`ra8_jof_read_tile()`), resident decoded pixels bounded
 * by the cache cell budget regardless of the page size, **no downscaling**.
 *
 * @par Size-thresholded front end (design: image_pipeline_unification.md S5)
 * Tiling a small page is pure overhead -- a footer, an index, per-tile framing
 * and a cache lookup to save nothing, because the whole image is resident after
 * the first pan anyway. The reader therefore keeps the existing whole-decode
 * fast path for pages under a resident-budget threshold (expressed in *decoded
 * bytes*, not pixels -- ::ra8_comic_tiles_over_budget) and only routes a page
 * whose decoded footprint exceeds the budget through this tile path. Pages that
 * already fit render byte-identically to before.
 *
 * @par One page at a time (single source, epoch-keyed)
 * A comic reader shows one page; a page turn re-imports. ::ra8_comic_tiles_import
 * resets the atlas store and rebinds a single source, bumping an internal epoch
 * that namespaces the tile-cache key so a re-import can never return a stale tile
 * of the previous page. All storage is caller-owned; there is no heap (NASA P10
 * Rule 3).
 *
 * @code
 * ra8_comic_tile_reader_t r = {};
 * ra8_comic_tiles_init(&r, &tile_cache_storage, scratch, sizeof scratch);
 *
 * uint16_t w = 0U;
 * uint16_t h = 0U;
 * uint64_t decoded = 0U;
 * if ((ra8_comic_tiles_footprint(enc, enc_len, &w, &h, &decoded) == k_ra8_ok) &&
 *     ra8_comic_tiles_over_budget(decoded, k_resident_budget_bytes)) {
 *   ra8_comic_tiles_import(&r, enc, enc_len, &import_cfg);  // tile it
 *   ra8_tile_t tile = {};
 *   ra8_comic_tiles_tile(&r, tile_x, tile_y, &tile);        // page/zoom a sub-rect
 *   // ... blit tile.pixels ...
 *   ra8_comic_tiles_release(&r, tile.pixels);
 * } else {
 *   ra8_img_decode_blit(&arena, enc, enc_len, 0, 0, box_w, box_h, nullptr, nullptr);
 * }
 * @endcode
 *
 * @note Not thread-safe; the single-threaded reader loop serialises access.
 * @note The JOF producer transcodes baseline JPEG / non-interlaced 8-bit PNG (and
 *       WebP when a `webp_work` arena is supplied); a page in any other encoding
 *       reports `k_ra8_err_not_supported` from ::ra8_comic_tiles_footprint /
 *       ::ra8_comic_tiles_import and the caller keeps the whole-decode path.
 *
 * @see ra8_jof.h                The JOF atlas format + tile reader.
 * @see ra8_jof_produce.h        The import-time transcode producer.
 * @see ra8_tile_cache.h         The decode-on-demand tile cache.
 * @see ra8_epub_img_tiles.h     The EPUB tile binder this mirrors.
 * @see ra8_comic.h              The CBZ/CBR page reader that supplies the bytes.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since Version 0.1.0
 *
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_jof.h"
#include "ra8_jof_produce.h"
#include "ra8_tile_cache.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @enum ra8_comic_tiles_const_t
 * @brief Fixed constants for the comic tiling budget estimate.
 * @details The whole-decode fast path decodes to RGB (`stb_image`), so a page's
 *          worst-case resident footprint is estimated at four bytes per pixel --
 *          an over-estimate that keeps a marginal page on the safe (tiling) side
 *          of the threshold rather than risking a whole-decode arena overflow.
 * @since Version 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_comic_tiles_decoded_bpp = 4U, /**< Bytes/pixel used for the budget estimate. */
} ra8_comic_tiles_const_t;

/**
 * @struct ra8_comic_tiles_import_cfg_t
 * @brief Import-time transcode knobs, work arena, and destination atlas store.
 *
 * @details Mirrors the producer configuration (`ra8_jof_produce.h`) minus the
 *          source (the import path streams the caller's encoded page bytes).
 *          Size @p work with `ra8_jof_work_bytes()` over the same caps and tile
 *          geometry. @p webp_work is the optional whole-frame arena for WebP
 *          pages (`ra8_jof_webp_work_bytes()`); leave it NULL to fail-closed
 *          reject WebP pages exactly as a JPEG/PNG-only reader always did.
 *          @p atlas is the append-only memstore backing the produced JOF atlas
 *          lives in and is paged from; it must out-live every tile fetch.
 *
 * @invariant `tile_w` / `tile_h` non-zero; `work` covers `work_cap`; `atlas`
 *            covers `atlas_cap`.
 * @invariant `webp_work` is NULL, or it covers `webp_work_cap` bytes.
 * @see ra8_comic_tiles_import()
 * @since Version 0.1.0
 */
typedef struct {
  uint16_t tile_w;        /**< Tile width, pixels (>= 1).                      */
  uint16_t tile_h;        /**< Tile height, pixels (>= 1).                     */
  uint8_t  codec;         /**< ::ra8_jof_codec_t member.                       */
  uint16_t max_width;     /**< Width budget cap (0 = format cap).              */
  uint16_t max_height;    /**< Height budget cap (0 = format cap).             */
  uint8_t* work;          /**< Producer streaming work arena (JPEG/PNG).       */
  size_t   work_cap;      /**< Arena size (ra8_jof_work_bytes()).              */
  uint8_t* webp_work;     /**< WebP whole-frame arena, or NULL to reject WebP. */
  size_t   webp_work_cap; /**< WebP arena size (ra8_jof_webp_work_bytes()).    */
  uint8_t* atlas;         /**< Atlas memstore backing (out-lives tile fetch).  */
  size_t   atlas_cap;     /**< Capacity of @p atlas in bytes.                  */
} ra8_comic_tiles_import_cfg_t;

/**
 * @struct ra8_comic_tile_reader_t
 * @brief One comic tile reader: owned tile cache, atlas store, bound page.
 *
 * @details Caller-owned; zero-initialise before ::ra8_comic_tiles_init. Holds a
 *          ::ra8_tile_cache whose decode-on-miss reads + decodes one JOF tile
 *          through `ra8_jof_read_tile()` off @ref store, the parsed geometry of
 *          the currently-bound page (@ref info), the stored-tile staging buffer
 *          (@ref scratch), and the @ref epoch that namespaces the tile-cache key
 *          per import so a page turn cannot surface a stale tile.
 *
 * @invariant `bound` is true only between a successful import and the next reset.
 * @invariant Tile-cache keys carry `image_id == epoch` of the bound page.
 * @see ra8_comic_tiles_init()
 * @since Version 0.1.0
 */
typedef struct {
  ra8_tile_cache_t   cache;       /**< Owned tile cache (decode wired to this). */
  ra8_jof_memstore_t store;       /**< Atlas backing for the bound page.        */
  ra8_jof_info_t     info;        /**< Parsed geometry of the bound page.       */
  uint8_t*           scratch;     /**< Stored-tile staging for deflate atlases. */
  uint32_t           scratch_cap; /**< Capacity of @ref scratch.                */
  uint32_t           epoch;       /**< Import counter; the tile-cache key id.   */
  bool               bound;       /**< True while a page atlas is imported.     */
} ra8_comic_tile_reader_t;

/**
 * @brief Read a comic page's decoded footprint from its encoded header (#344).
 *
 * @details Sniffs the encoded page's dimensions without decoding its body (via
 *          `ra8_jof_probe_dims()`, the exact JPEG/PNG/WebP set the producer
 *          accepts) and reports the worst-case decoded byte count
 *          (`w * h * k_ra8_comic_tiles_decoded_bpp`) the whole-decode arena
 *          would have to hold. The caller compares it against its resident
 *          budget with ::ra8_comic_tiles_over_budget to choose the tile path or
 *          the whole-decode fast path.
 *
 * @param[in]  enc               Encoded page bytes (non-NULL).
 * @param[in]  len               Readable byte count at @p enc (> 0).
 * @param[out] out_w             Receives the page width in pixels (non-NULL).
 * @param[out] out_h             Receives the page height in pixels (non-NULL).
 * @param[out] out_decoded_bytes Receives the worst-case decoded footprint (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Footprint reported; all outputs written.
 * @retval k_ra8_err_null_ptr      A required pointer argument was NULL.
 * @retval k_ra8_err_invalid_size  @p len is 0, or a probed dimension is out of range.
 * @retval k_ra8_err_not_supported The page is not a JPEG/PNG/WebP the producer tiles.
 * @retval k_ra8_err_*             Propagated from the per-format probe.
 *
 * @pre @p enc holds @p len readable bytes.
 * @pre @p out_w, @p out_h and @p out_decoded_bytes are writable.
 * @post On k_ra8_ok every output is populated from the page header.
 * @post On any error no output is relied upon.
 *
 * @note Thread-safe: pure read of @p enc.
 * @see ra8_comic_tiles_over_budget()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_comic_tiles_footprint(const uint8_t* enc,
                                                  size_t         len,
                                                  uint16_t*      out_w,
                                                  uint16_t*      out_h,
                                                  uint64_t*      out_decoded_bytes);

/**
 * @brief Decide whether a page's decoded footprint exceeds the resident budget.
 *
 * @details The size-threshold decision expressed in decoded bytes: a page is
 *          worth tiling only when the budget is a real (non-zero) ceiling AND
 *          the page's decoded footprint would overrun it. A zero budget means
 *          "never tile" (the whole-decode path is unconditional), so a
 *          mis-configured budget fails safe rather than tiling everything.
 *
 * @param[in] decoded_bytes Page's worst-case decoded footprint (from footprint()).
 * @param[in] budget_bytes  Resident decode budget in bytes (0 = never tile).
 *
 * @return Whether the page should be routed through the tile path.
 * @retval true  The budget is non-zero and the page overruns it.
 * @retval false The budget is zero, or the page fits it.
 *
 * @pre @p decoded_bytes came from ::ra8_comic_tiles_footprint (or is 0).
 * @pre @p budget_bytes is the caller's resident decode budget.
 * @post No state is modified (pure function).
 * @post The result depends only on the two arguments.
 *
 * @note Thread-safe: pure.
 * @see ra8_comic_tiles_footprint()
 * @since Version 0.1.0
 */
[[nodiscard]] bool ra8_comic_tiles_over_budget(uint64_t decoded_bytes, uint64_t budget_bytes);

/**
 * @brief Initialise a comic tile reader over caller-supplied tile-cache storage.
 *
 * @details Wires @p storage into the owned ::ra8_tile_cache whose decode-on-miss
 *          is this module's JOF tile reader. The @p storage `decode` /
 *          `decode_ctx` fields are ignored (the reader sets them); every other
 *          field (cell memory + geometry + key/dim/meta arrays + hash buckets)
 *          is the caller's and must out-live the reader. @p scratch stages one
 *          stored (compressed) tile during a deflate decode-on-miss: size it
 *          with `ra8_jof_stored_bound()` over the cell size; a reader paging
 *          only raw atlases may pass NULL/0.
 *
 * @param[out] r           Reader to populate (zero-initialised by the caller).
 * @param[in]  storage     Tile-cache storage config; `decode`/`decode_ctx` unused.
 * @param[in]  scratch     Stored-tile staging buffer (may be NULL for raw-only).
 * @param[in]  scratch_cap Capacity of @p scratch, bytes.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Reader ready; no page imported yet.
 * @retval k_ra8_err_null_ptr     @p r or @p storage (or a required array) NULL.
 * @retval k_ra8_err_invalid_size A zero cell/bucket geometry in @p storage.
 *
 * @pre @p r points at zeroed storage.
 * @pre @p storage arrays and @p scratch out-live the reader.
 * @post On success the cache is empty and no page is bound.
 * @post On failure @p r is left unusable.
 *
 * @note Not thread-safe.
 * @see ra8_comic_tiles_import()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_comic_tiles_init(ra8_comic_tile_reader_t*    r,
                                             const ra8_tile_cache_cfg_t* storage,
                                             uint8_t*                    scratch,
                                             uint32_t                    scratch_cap);

/**
 * @brief Transcode one encoded comic page to a JOF atlas and bind it for tiling.
 *
 * @details Streams @p enc through `ra8_jof_produce()` into @p cfg->atlas (a
 *          memstore), validates the produced atlas with `ra8_jof_parse()`, and
 *          binds it as the reader's single source under a fresh epoch. After a
 *          successful import ::ra8_comic_tiles_tile pages the page's
 *          full-resolution tiles on demand -- the #344 goal: a comic page larger
 *          than the whole-decode arena renders without a whole-image decode and
 *          without downscaling. Re-importing (a page turn) resets the store and
 *          bumps the epoch so no stale tile of the previous page survives.
 *
 * @param[in,out] r       Reader from ::ra8_comic_tiles_init.
 * @param[in]     enc     Encoded page bytes (JPEG/PNG/WebP; non-NULL).
 * @param[in]     enc_len Length of @p enc (> 0).
 * @param[in]     cfg     Transcode knobs + work arena + atlas store (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Page transcoded and bound; tiles servable.
 * @retval k_ra8_err_null_ptr          A required pointer argument was NULL.
 * @retval k_ra8_err_invalid_arg       Zero tile geometry or unknown codec.
 * @retval k_ra8_err_invalid_size      Page exceeds the caps, grid exceeds the
 *                                     tile cap, or an arena/store is too small.
 * @retval k_ra8_err_not_supported     Page not JPEG/PNG/WebP, WebP with no
 *                                     `webp_work`, or an unsupported variant.
 * @retval k_ra8_err_protocol_error    Malformed / hostile source structure.
 * @retval k_ra8_err_validation_failed Producer or atlas validation failed.
 * @retval k_ra8_err_*                 Propagated from the producer / store.
 *
 * @pre @p r came from ::ra8_comic_tiles_init.
 * @pre @p cfg->work is sized per `ra8_jof_work_bytes()` for the same caps.
 * @post On success the page's tiles are servable and `r->bound` is true.
 * @post On any error `r->bound` is false and no tile may be fetched.
 *
 * @note Not thread-safe.
 * @see ra8_comic_tiles_tile()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_comic_tiles_import(ra8_comic_tile_reader_t*            r,
                                               const uint8_t*                      enc,
                                               size_t                              enc_len,
                                               const ra8_comic_tiles_import_cfg_t* cfg);

/**
 * @brief Report the bound page's parsed atlas geometry (#344).
 *
 * @param[in]  r        Reader with a page bound by ::ra8_comic_tiles_import.
 * @param[out] out_info Receives the atlas geometry (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Geometry reported.
 * @retval k_ra8_err_null_ptr      @p r or @p out_info is NULL.
 * @retval k_ra8_err_invalid_state No page is bound.
 *
 * @pre @p r is initialised; @p out_info is writable.
 * @pre A page was bound via ::ra8_comic_tiles_import.
 * @post On success `*out_info` holds the bound page geometry.
 * @post On failure `*out_info` is unmodified.
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_comic_tiles_info(const ra8_comic_tile_reader_t* r,
                                             ra8_jof_info_t*                out_info);

/**
 * @brief Get (and pin) one decoded tile of the bound page (#344).
 *
 * @details Builds the tile-cache key `(epoch, tile_x, tile_y)` and fetches it
 *          through the owned cache. On a miss the tile's stored stream is read
 *          off the atlas store and decoded into one cache cell
 *          (`ra8_jof_read_tile()`); on a hit the cell is reused. The returned
 *          pixels are tightly packed (`out_tile->width * bpp` bytes per row) and
 *          stay valid until ::ra8_comic_tiles_release. Edge tiles report their
 *          true (smaller) size. This is the sub-rect access the loupe needs:
 *          magnifying a region fetches only the tiles under it, full resolution.
 *
 * @param[in]  r        Reader with a page bound.
 * @param[in]  tile_x   Tile column, `[0, info.tile_cols)`.
 * @param[in]  tile_y   Tile row, `[0, info.tile_rows)`.
 * @param[out] out_tile Receives the pinned tile view (non-NULL).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Tile resident + pinned; `*out_tile` set.
 * @retval k_ra8_err_null_ptr          @p r or @p out_tile is NULL.
 * @retval k_ra8_err_invalid_state     No page is bound.
 * @retval k_ra8_err_out_of_range      @p tile_x / @p tile_y outside the grid.
 * @retval k_ra8_err_no_mem            Every cell is pinned, or the tile exceeds
 *                                     the cell / scratch budget.
 * @retval k_ra8_err_validation_failed The atlas backing is corrupt.
 *
 * @pre @p r has a page bound via ::ra8_comic_tiles_import.
 * @pre The caller will ::ra8_comic_tiles_release the returned tile.
 * @post On success the cell's pin count grew by one.
 * @post On any error no new pin is held.
 *
 * @note Not thread-safe.
 * @see ra8_comic_tiles_release()
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_comic_tiles_tile(ra8_comic_tile_reader_t* r,
                                             uint16_t                 tile_x,
                                             uint16_t                 tile_y,
                                             ra8_tile_t*              out_tile);

/**
 * @brief Release one pin taken by ::ra8_comic_tiles_tile (#344).
 *
 * @param[in] r      Reader with a page bound.
 * @param[in] pixels The `pixels` pointer from a returned ::ra8_tile_t.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Pin released.
 * @retval k_ra8_err_null_ptr    @p r or @p pixels is NULL.
 * @retval k_ra8_err_invalid_arg @p pixels is not a pinned cell of this reader.
 *
 * @pre @p pixels came from ::ra8_comic_tiles_tile on this reader.
 * @pre @p r is initialised.
 * @post On success the cell's pin count dropped by one.
 * @post On any error no state changed.
 *
 * @note Not thread-safe.
 * @since Version 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_comic_tiles_release(ra8_comic_tile_reader_t* r, const uint8_t* pixels);

#ifdef __cplusplus
}
#endif
