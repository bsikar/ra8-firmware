/**
 * @file epub_img_tiles.h
 * @brief Page a large in-EPUB image through ra8_tile_cache + a real reflow
 *        `<img>` loader off the streaming reader (#231).
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / EPUB] {World: NS}
 *
 * @details
 * A full-page manga scan in an all-image EPUB inflates to tens of megabytes --
 * too big to decode whole into the ~10 MB working set. This module supplies
 * the #231 runtime pieces, all bounded-RAM by construction and all reading
 * off the streaming reader (`epub_open_streamed`) so the whole archive is
 * never resident:
 *
 *   1. **Tile binder** (`epub_tile_binder_*`): pages JOF tile atlases
 *      (`jof.h` -- the display-native band-tile format shared with
 *      the longstrip scroll #289 and the #290 codec policy) through
 *      ::ra8_tile_cache, decode-on-demand keyed by `(image_id, tile_x,
 *      tile_y)`. An atlas backs onto either a *stored* archive entry
 *      (`epub_tile_binder_add()`, host-baked books) or any external
 *      pread seam (`epub_tile_binder_add_ext()` -- an SDRAM memstore or
 *      SD file holding an atlas produced on device). Resident decoded
 *      pixels stay bounded by the cache's cell budget regardless of the
 *      image size, with no downscaling.
 *   2. **Import-time transcode** (`epub_tile_binder_import()`): the
 *      #231 producer wired to the open path. Resolves a manifest href,
 *      streams the encoded JPEG/PNG (or, with a `webp_work` arena, WebP)
 *      through `jof_produce()` into a caller-supplied atlas store,
 *      and registers the result -- after which a page larger than SDRAM at
 *      native resolution renders full-res via decode-on-demand tiles. Every
 *      source codec converges on the one JOF container (#290). An entry that
 *      already IS a stored JOF atlas registers in place with no transcode.
 *   3. **Reflow `<img>` loader** (`epub_reflow_img_load`): the real
 *      `reflow_image_loader_fn` -- resolves an `<img src>` href to an
 *      EPUB manifest resource and returns its encoded bytes in a
 *      *caller-owned bounded scratch*. Images that fit the scratch (covers,
 *      figures) render exactly as before; an image larger than the scratch
 *      is reported unavailable rather than blowing the budget (the tile
 *      binder is the path for those).
 *
 *
 * @see jof.h          The JOF atlas format + reader.
 * @see jof_produce.h  The import-time transcode producer.
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "epub.h"
#include "jof.h"
#include "jof_produce.h"
#include "ra8_err.h"
#include "ra8_tile_cache.h"

/**
 * @enum epub_tile_limits_t
 * @brief Binder capacity limits.
 *
 * @details `k_epub_tile_max_sources` is how many distinct images one
 *          binder can serve from a single cache.
 */
typedef enum : uint8_t {
  k_epub_tile_max_sources = 8U, /**< Distinct images per binder. */
} epub_tile_limits_t;

/**
 * @struct epub_tile_source_t
 * @brief One registered atlas image (private to the binder; treat as opaque).
 *
 * @details Book-backed sources (`book != NULL`) read tiles off a stored
 *          archive entry at `path`; external sources read through their own
 *          pread seam (`pread`/`pread_ctx`).
 *
 * @invariant Exactly one of `book` or `pread` is non-NULL while registered.
 * @since 0.1.0
 */
typedef struct {
  epub_book_t* book;                      /**< Owning book, or NULL for external atlas. */
  char         path[k_epub_max_path_len]; /**< Entry path (book-backed only).           */
  jof_pread_fn pread;                     /**< External atlas read seam, or NULL.       */
  void*        pread_ctx;                 /**< Context for `pread`.                     */
  uint64_t     total_size;                /**< Atlas byte length (backing size).        */
  uint32_t     image_id;                  /**< Tile-cache key namespace for this image. */
  jof_info_t   info;                      /**< Parsed + validated atlas geometry.       */
} epub_tile_source_t;

/**
 * @struct epub_tile_binder_t
 * @brief Binds up to ::k_epub_tile_max_sources images to one tile cache.
 *
 * @details Owns a ::ra8_tile_cache whose decode-on-miss reads + decodes one
 *          JOF tile through `jof_read_tile()`. Caller-owned;
 *          zero-initialise before `epub_tile_binder_init()`.
 *
 * @invariant Every valid `sources[i]` has a distinct `image_id`.
 * @since 0.1.0
 */
typedef struct {
  ra8_tile_cache_t cache; /**< Owned tile cache (decode wired to the binder). */
  /** Registered images. */
  epub_tile_source_t sources[k_epub_tile_max_sources];
  /** Registered image count. */
  uint8_t  source_count;
  uint8_t* scratch;     /**< Stored-tile staging for deflate atlases. */
  uint32_t scratch_cap; /**< Capacity of `scratch`.                   */
} epub_tile_binder_t;

/**
 * @struct epub_img_loader_t
 * @brief Context for the real reflow `<img>` loader (`epub_reflow_img_load`).
 *
 * @details Binds an open book plus a caller-owned scratch buffer; the scratch
 *          capacity is the hard RAM ceiling on a single decoded `<img>` resource.
 *
 * @invariant `book != NULL`, `scratch != NULL`, `scratch_cap > 0` while bound.
 * @since 0.1.0
 */
typedef struct {
  epub_book_t* book;        /**< Book to resolve `<img src>` hrefs against. */
  uint8_t*     scratch;     /**< Caller-owned encoded-bytes scratch.        */
  size_t       scratch_cap; /**< Scratch capacity (bounded RAM ceiling).    */
} epub_img_loader_t;

/**
 * @struct epub_atlas_store_t
 * @brief Where an on-device-produced atlas lives: sink to write, pread to
 *        read back (DIP -- an SDRAM memstore and an SD file both fit).
 *
 * @details The import path writes the transcoded atlas through `sink` and
 *          registers the image over `pread`; both must address the same
 *          backing bytes. `jof_memstore_t` provides a matching
 *          RAM-backed pair.
 *
 * @invariant `sink` and `pread` address the same backing store.
 * @see epub_tile_binder_import()
 * @since 0.1.0
 */
typedef struct {
  jof_sink_fn  sink;      /**< Append-only atlas writer.          */
  void*        sink_ctx;  /**< Context for `sink`.                */
  jof_pread_fn pread;     /**< Read-back seam for the same bytes. */
  void*        pread_ctx; /**< Context for `pread`.               */
} epub_atlas_store_t;

/**
 * @struct epub_atlas_import_cfg_t
 * @brief Import-time transcode knobs: tile geometry, budget caps, work arena
 *        and the destination store.
 *
 * @details Mirrors the producer configuration (`jof_produce.h`)
 *          minus the source, which the import path streams from the archive
 *          entry itself. Size `work` with `jof_work_bytes()` over
 *          the same caps. `webp_work` is the optional whole-frame arena for
 *          WebP manifest images (size it with
 *          `jof_webp_work_bytes()`); leave it NULL to fail-closed
 *          reject WebP entries the way JPEG/PNG-only importers always did.
 *
 * @invariant `tile_w`/`tile_h` non-zero; `work` covers `work_cap`.
 * @invariant `webp_work` is NULL, or it covers `webp_work_cap`.
 * @see epub_tile_binder_import()
 * @since 0.1.0
 */
typedef struct {
  uint16_t           tile_w;        /**< Tile width, pixels (>= 1).                      */
  uint16_t           tile_h;        /**< Tile height, pixels (>= 1).                     */
  uint8_t            codec;         /**< ::jof_codec_t member.                           */
  uint16_t           max_width;     /**< Width budget cap (0 = format cap).              */
  uint16_t           max_height;    /**< Height budget cap (0 = format cap).             */
  uint8_t*           work;          /**< Producer streaming work arena (JPEG/PNG).       */
  size_t             work_cap;      /**< Arena size (jof_work_bytes()).                  */
  uint8_t*           webp_work;     /**< WebP whole-frame arena, or NULL to reject WebP. */
  size_t             webp_work_cap; /**< WebP arena size (jof_webp_work_bytes()).        */
  epub_atlas_store_t store;         /**< Destination atlas store.                        */
} epub_atlas_import_cfg_t;

/**
 * @brief Initialise a tile binder over caller-supplied tile-cache storage (#231).
 *
 * @details
 * Wires @p storage into an owned ::ra8_tile_cache whose decode-on-miss is this
 * module's JOF tile reader. The @p storage `decode` / `decode_ctx` fields are
 * ignored (the binder sets them); all other fields (cell memory + geometry +
 * key/dim/meta arrays + hash buckets) are the caller's and must out-live the
 * binder. @p scratch stages one stored (compressed) tile during a deflate
 * decode-on-miss: size it with `jof_stored_bound()` over the cell
 * size; a binder serving only raw atlases may pass NULL/0.
 *
 * @param[out] binder      Binder to populate (zero-initialised by the caller).
 * @param[in]  storage     Tile-cache storage config; `decode`/`decode_ctx` unused.
 * @param[in]  scratch     Stored-tile staging buffer (may be NULL for raw-only).
 * @param[in]  scratch_cap Capacity of @p scratch, bytes.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok               Binder ready; no images registered yet.
 * @retval k_ra8_err_null_ptr     @p binder or @p storage (or a required array) NULL.
 * @retval k_ra8_err_invalid_size A zero cell/bucket geometry in @p storage.
 *
 * @pre @p binder points at zeroed storage.
 * @pre @p storage arrays and @p scratch out-live the binder.
 * @post On success the cache is empty and no source is registered.
 * @post On failure @p binder is left unusable.
 * @note Not thread-safe.
 * @see epub_tile_binder_add()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t epub_tile_binder_init(epub_tile_binder_t*         binder,
                                              const ra8_tile_cache_cfg_t* storage,
                                              uint8_t*                    scratch,
                                              uint32_t                    scratch_cap);

/**
 * @brief Register a stored in-archive JOF atlas entry under @p image_id (#231).
 *
 * @details
 * Resolves @p path (OPF-relative, then archive-rooted), measures the entry,
 * and validates the atlas structure via `jof_parse()` over the
 * entry pread seam. The entry must be *stored* (uncompressed) in the ZIP so
 * tiles can be windowed with positioned reads. After this, tiles of the
 * image are fetched with `epub_tile_binder_get()` keyed by @p image_id.
 *
 * @param[in,out] binder   Initialised binder.
 * @param[in]     book     Open book whose archive holds @p path.
 * @param[in]     path     Atlas entry path, OPF-relative or archive-rooted.
 * @param[in]     image_id Caller-chosen id (must be unique within the binder).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                    Image registered.
 * @retval k_ra8_err_null_ptr          @p binder, @p book, or @p path is NULL.
 * @retval k_ra8_err_no_mem            The binder's source table is full.
 * @retval k_ra8_err_invalid_arg       @p image_id already registered / bad path length.
 * @retval k_ra8_err_validation_failed The entry is not a structurally valid atlas.
 * @retval k_ra8_err_not_supported     The entry is DEFLATE-compressed in the ZIP.
 * @retval other                       Propagated from the entry reader.
 *
 * @pre @p binder came from `epub_tile_binder_init()`.
 * @pre `path` is longer than 0 and shorter than ::k_epub_max_path_len.
 * @post On success `source_count` grew by one and the geometry is queryable.
 * @post On any error the source table is unchanged.
 * @note Not thread-safe.
 * @see epub_tile_binder_get()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t epub_tile_binder_add(epub_tile_binder_t* binder,
                                             epub_book_t*        book,
                                             const char*         path,
                                             uint32_t            image_id);

/**
 * @brief Register an externally-backed JOF atlas under @p image_id (#231).
 *
 * @details
 * The external seam serves atlases that live outside the archive: an SDRAM
 * memstore or SD file filled by the import-time transcode
 * (`epub_tile_binder_import()` calls this itself), or any other backing.
 * The atlas structure is validated via `jof_parse()` before the
 * source is accepted.
 *
 * @param[in,out] binder     Initialised binder.
 * @param[in]     pread      Atlas read seam (non-NULL).
 * @param[in]     pread_ctx  Context for @p pread (must out-live the binder).
 * @param[in]     total_size Atlas byte length.
 * @param[in]     image_id   Caller-chosen id (unique within the binder).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                    Image registered.
 * @retval k_ra8_err_null_ptr          @p binder or @p pread is NULL.
 * @retval k_ra8_err_no_mem            The binder's source table is full.
 * @retval k_ra8_err_invalid_arg       @p image_id already registered.
 * @retval k_ra8_err_invalid_size      @p total_size cannot hold an atlas.
 * @retval k_ra8_err_validation_failed The backing is not a valid atlas.
 * @retval other                       Propagated from @p pread.
 *
 * @pre @p binder came from `epub_tile_binder_init()`.
 * @pre @p pread serves the atlas bytes for `[0, total_size)`.
 * @post On success `source_count` grew by one and the geometry is queryable.
 * @post On any error the source table is unchanged.
 * @note Not thread-safe.
 * @see epub_tile_binder_import()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t epub_tile_binder_add_ext(epub_tile_binder_t* binder,
                                                 jof_pread_fn        pread,
                                                 void*               pread_ctx,
                                                 uint64_t            total_size,
                                                 uint32_t            image_id);

/**
 * @brief Import a manifest image through the transcode producer and register
 *        it for tile paging (#231 -- the open-path wiring).
 *
 * @details
 * Resolves @p href against the book, then:
 *   1. If the entry is already a *stored* JOF atlas, registers it in place
 *      (`epub_tile_binder_add()`) -- the host-baked fast path, no
 *      transcode and no store writes.
 *   2. Otherwise streams the entry's encoded bytes through
 *      `jof_produce()` (JPEG/PNG in bounded stripes so the whole
 *      decoded image is never resident; WebP whole-frame through the
 *      `cfg->webp_work` arena) into @p cfg->store, then registers the produced
 *      atlas via `epub_tile_binder_add_ext()`.
 *
 * After a successful import, `epub_tile_binder_get()` pages the image's
 * full-resolution tiles on demand -- the #231 goal: a manga page larger than
 * SDRAM at native resolution renders without whole-image decode and without
 * downscaling.
 *
 * @param[in,out] binder   Initialised binder.
 * @param[in]     book     Open book (streamed or resident).
 * @param[in]     href     Manifest image href, NUL-terminated.
 * @param[in]     image_id Caller-chosen id (unique within the binder).
 * @param[in]     cfg      Transcode knobs + work arena + atlas store.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                    Image registered (either path).
 * @retval k_ra8_err_null_ptr          A required pointer is NULL.
 * @retval k_ra8_err_invalid_arg       Bad href length / duplicate id / bad cfg.
 * @retval k_ra8_err_no_mem            Source table full, or the store filled.
 * @retval k_ra8_err_not_found         @p href resolves to no archive entry.
 * @retval k_ra8_err_not_supported     The entry is not JPEG/PNG/WebP/atlas, a
 *                                     WebP with no `webp_work` arena, or an
 *                                     unsupported variant.
 * @retval k_ra8_err_invalid_size      Source exceeds the caps / arena too small.
 * @retval k_ra8_err_protocol_error    Malformed / hostile source structure.
 * @retval k_ra8_err_validation_failed Producer or atlas validation failed.
 * @retval other                       Propagated from the reader / store.
 *
 * @pre @p binder came from `epub_tile_binder_init()`.
 * @pre @p cfg->work is sized per `jof_work_bytes()`.
 * @post On success the image's tiles are servable by @p image_id.
 * @post On any error the source table is unchanged (a partial store write
 *       is abandoned and must be reset by the caller before reuse).
 * @note Not thread-safe.
 * @see epub_tile_binder_get()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t epub_tile_binder_import(epub_tile_binder_t*            binder,
                                                epub_book_t*                   book,
                                                const char*                    href,
                                                uint32_t                       image_id,
                                                const epub_atlas_import_cfg_t* cfg);

/**
 * @brief Report a registered image's parsed geometry (#231).
 *
 * @param[in]  binder   Binder with @p image_id registered.
 * @param[in]  image_id Image to query.
 * @param[out] out_info Receives the atlas geometry on success.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok            Geometry reported.
 * @retval k_ra8_err_null_ptr  @p binder or @p out_info is NULL.
 * @retval k_ra8_err_not_found @p image_id is not registered.
 *
 * @pre @p binder is initialised; @p out_info is writable.
 * @pre @p image_id was registered via an add/import call.
 * @post On success `*out_info` holds the image geometry.
 * @post On failure `*out_info` is unmodified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
epub_tile_binder_info(const epub_tile_binder_t* binder, uint32_t image_id, jof_info_t* out_info);

/**
 * @brief Get (and pin) one decoded tile of a registered image (#231).
 *
 * @details
 * Builds the tile-cache key `(image_id, tile_x, tile_y)` and fetches it through
 * the owned cache. On a miss the tile's stored stream is read off the atlas
 * backing and decoded into one cache cell (`jof_read_tile()`); on a
 * hit the cell is reused. The returned pixels are tightly packed
 * (`out_tile->width * bpp` bytes per row) and stay valid until
 * `epub_tile_binder_put()`. Edge tiles report their true (smaller) size.
 *
 * @param[in]  binder   Binder with @p image_id registered.
 * @param[in]  image_id Image to fetch a tile of.
 * @param[in]  tile_x   Tile column, `[0, tile_cols)`.
 * @param[in]  tile_y   Tile row, `[0, tile_rows)`.
 * @param[out] out_tile Receives the pinned tile view.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                 Tile resident + pinned; `*out_tile` set.
 * @retval k_ra8_err_null_ptr       @p binder or @p out_tile is NULL.
 * @retval k_ra8_err_not_found      @p image_id is not registered.
 * @retval k_ra8_err_out_of_range   @p tile_x / @p tile_y is outside the grid.
 * @retval k_ra8_err_no_mem         Every cell is pinned, or the tile exceeds
 *                                  the cell/scratch budget.
 * @retval k_ra8_err_validation_failed The atlas backing is corrupt.
 *
 * @pre @p binder is initialised and @p image_id registered.
 * @pre The caller will `epub_tile_binder_put()` the returned tile.
 * @post On success the cell's pin count grew by one.
 * @post On any error no new pin is held.
 * @note Not thread-safe.
 * @see epub_tile_binder_put()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t epub_tile_binder_get(epub_tile_binder_t* binder,
                                             uint32_t            image_id,
                                             uint16_t            tile_x,
                                             uint16_t            tile_y,
                                             ra8_tile_t*         out_tile);

/**
 * @brief Release one pin taken by `epub_tile_binder_get()` (#231).
 *
 * @param[in] binder Initialised binder.
 * @param[in] pixels The `pixels` pointer from a returned ::ra8_tile_t.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              Pin released.
 * @retval k_ra8_err_null_ptr    @p binder or @p pixels is NULL.
 * @retval k_ra8_err_invalid_arg @p pixels is not a pinned cell of this binder.
 *
 * @pre @p pixels came from `epub_tile_binder_get()` on this binder.
 * @pre @p binder is initialised.
 * @post On success the cell's pin count dropped by one.
 * @post On any error no state changed.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t epub_tile_binder_put(epub_tile_binder_t* binder, const uint8_t* pixels);

/**
 * @brief Predictively warm the tiles one step ahead of a panning image (#341).
 *
 * @details
 * The image-render counterpart of ::book_src_prefetch_chapter's text
 * read-ahead: on a pan of a tiled page (the #231 all-image EPUB path, and the
 * comic-tiling path that shares this binder), warms the tile row or column just
 * beyond @p view in @p dir so the next tiles the viewport exposes are resident
 * before they are needed. The binder supplies the image's tile-grid extent to
 * ::ra8_tile_cache_prefetch_pan, which clamps the lead edge to the grid (a pan
 * already at the image edge warms nothing) and caps the count at @p max_tiles --
 * the caller's spare-capacity budget -- so read-ahead can neither exceed the
 * cache budget nor evict an on-screen tile. Best-effort and transparent: warming
 * changes only residency, never the bytes a later `epub_tile_binder_get()`
 * returns, so goldens hold.
 *
 * @param[in,out] binder     Initialised binder with @p image_id registered.
 * @param[in]     image_id   Image being panned.
 * @param[in]     view       The tiles the viewport currently straddles.
 * @param[in]     dir        Direction of viewport travel.
 * @param[in]     max_tiles  Residency budget: warm at most this many tiles.
 * @param[out]    out_warmed Tiles warmed by this call (may be NULL).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              The sweep ran (0 or more tiles warmed).
 * @retval k_ra8_err_null_ptr    @p binder or @p view is NULL.
 * @retval k_ra8_err_not_found   @p image_id is not registered.
 * @retval k_ra8_err_invalid_arg @p view is unordered or lies outside the grid.
 *
 * @pre @p binder came from `epub_tile_binder_init()`.
 * @pre @p view is a valid inclusive tile rectangle of @p image_id.
 * @post On success at most @p max_tiles lead-edge tiles are resident.
 * @post No on-screen (already-visible) tile is evicted by this call.
 * @note Not thread-safe. Single-threaded read-ahead only.
 * @see epub_tile_binder_get()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t epub_tile_binder_prefetch_pan(epub_tile_binder_t*    binder,
                                                      uint32_t               image_id,
                                                      const ra8_tile_rect_t* view,
                                                      ra8_tile_pan_dir_t     dir,
                                                      uint16_t               max_tiles,
                                                      uint16_t*              out_warmed);

/**
 * @brief Real reflow `<img>` byte loader off an EPUB book (#231).
 *
 * @details
 * A `reflow_image_loader_fn` (matching signature) that resolves @p href to an
 * EPUB manifest resource and returns its encoded bytes in the bound scratch
 * (`epub_get_resource`, which streams from the reader on demand). The scratch
 * capacity is the RAM ceiling: an image that does not fit is reported unavailable
 * (`k_ra8_err_no_mem`), which the reflow engine treats as "skip / placeholder"
 * rather than exceeding the budget. Wire it into the engine with
 * `reflow_set_image_loader()`, passing a ::epub_img_loader_t as @p ctx.
 *
 * @param[in]  ctx       A ::epub_img_loader_t* (book + scratch).
 * @param[in]  href      `<img src>` string (not NUL-terminated).
 * @param[in]  href_len  Length of @p href, bytes.
 * @param[out] out_bytes Receives a pointer to the encoded bytes (into the scratch).
 * @param[out] out_len   Receives the encoded byte count.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok            Resource resolved into the scratch.
 * @retval k_ra8_err_null_ptr  @p ctx, @p href, @p out_bytes, or @p out_len is NULL.
 * @retval k_ra8_err_invalid_arg @p href_len is 0 or exceeds the path buffer.
 * @retval k_ra8_err_no_mem    The resource does not fit the bound scratch.
 * @retval other               Propagated from `epub_get_resource`.
 *
 * @pre @p ctx binds a live book, a non-NULL scratch, and `scratch_cap > 0`.
 * @pre @p href holds @p href_len readable bytes.
 * @post On success `*out_bytes` / `*out_len` describe the scratch contents.
 * @post On any error `*out_len == 0` and the engine falls back to a placeholder.
 * @note Not thread-safe; the scratch is single-decode.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t epub_reflow_img_load(void*           ctx,
                                             const char*     href,
                                             uint32_t        href_len,
                                             const uint8_t** out_bytes,
                                             size_t*         out_len);

#ifdef __cplusplus
}
#endif
