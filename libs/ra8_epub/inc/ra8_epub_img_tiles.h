/**
 * @file ra8_epub_img_tiles.h
 * @brief Page a large in-EPUB image through ra8_tile_cache + a real reflow
 *        `<img>` loader off the streaming reader (#231).
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / EPUB] {World: NS}
 *
 * @details
 * A full-page manga scan in an all-image EPUB inflates to tens of megabytes -- too
 * big to decode whole into the ~10 MB working set, and (`stb_image` has no region
 * decode) impossible to decode a sub-rectangle of an arbitrary JPEG/PNG on device.
 * This module supplies the two runtime pieces #231 asks for, both bounded-RAM by
 * construction and both reading off the streaming reader (`ra8_epub_open_streamed`)
 * so the whole archive is never resident:
 *
 *   1. **Tile binder** (`ra8_epub_tile_binder_*`): pages a large *display-native*
 *      image entry through ::ra8_tile_cache, decode-on-demand keyed by
 *      `(image_id, tile_x, tile_y)`. The entry is a stored (uncompressed) raster
 *      atlas (::ra8_epub_tileimg -- see the on-disk header below), so a single
 *      tile is served by a handful of positioned reads (`ra8_epub_entry_pread`)
 *      into one cache cell; resident decoded pixels stay bounded by the cache's
 *      cell budget regardless of the image size, with no downscaling. The atlas is
 *      the runtime output of an import-time transcode (mirrors
 *      `ra8_rabook_pipeline`); producing it from an arbitrary encoded image is a
 *      separate step (it needs a region/banded pixel decoder) and is not in this
 *      module.
 *   2. **Reflow `<img>` loader** (`ra8_epub_reflow_img_load`): the real
 *      `ra8_reflow_image_loader_fn` -- resolves an `<img src>` href to an EPUB
 *      manifest resource and returns its encoded bytes in a *caller-owned bounded
 *      scratch*. Images that fit the scratch (covers, figures) render exactly as
 *      before (byte-identical); an image larger than the scratch is reported
 *      unavailable rather than blowing the budget (the tile binder is the path for
 *      those). This replaces the demo one-asset binders in the ereader apps.
 *
 * ## Tile-atlas on-disk header (::ra8_epub_tileimg, 16 bytes, little-endian)
 *
 * | Offset | Size | Field                                   |
 * |--------|------|-----------------------------------------|
 * | 0      | 4    | magic `"RTI1"`                          |
 * | 4      | 2    | image width, pixels                     |
 * | 6      | 2    | image height, pixels                    |
 * | 8      | 2    | tile width, pixels                      |
 * | 10     | 2    | tile height, pixels                     |
 * | 12     | 1    | bytes per pixel (1=gray8 .. 4=RGBA)     |
 * | 13     | 3    | reserved (zero)                         |
 *
 * Pixel data follows the header, row-major, `width * bpp` bytes per row.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_epub.h"
#include "ra8_err.h"
#include "ra8_tile_cache.h"

/**
 * @enum ra8_epub_tileimg_t
 * @brief On-disk tile-atlas header geometry + limits.
 *
 * @details The 16-byte header (see the file comment) precedes the row-major pixel
 *          data. `k_ra8_epub_tile_bpp_max` caps the bytes-per-pixel the binder will
 *          accept; `k_ra8_epub_tile_max_sources` is how many distinct images one
 *          binder can serve from a single cache.
 */
typedef enum : uint8_t {
  k_ra8_epub_tileimg_hdr_bytes = 16U, /**< Header length before pixel data. */
  k_ra8_epub_tileimg_magic_len = 4U,  /**< Magic string length.             */
  k_ra8_epub_tile_bpp_max      = 4U,  /**< Max bytes per pixel accepted.    */
  k_ra8_epub_tile_max_sources  = 8U,  /**< Distinct images per binder.      */
} ra8_epub_tileimg_t;

/**
 * @enum ra8_epub_tileimg_ofs_t
 * @brief Byte offsets of each little-endian field in the tile-atlas header.
 */
typedef enum : uint8_t {
  k_ra8_epub_tileimg_ofs_magic  = 0U,  /**< Magic `"RTI1"`.        */
  k_ra8_epub_tileimg_ofs_width  = 4U,  /**< uint16 image width.    */
  k_ra8_epub_tileimg_ofs_height = 6U,  /**< uint16 image height.   */
  k_ra8_epub_tileimg_ofs_tile_w = 8U,  /**< uint16 tile width.     */
  k_ra8_epub_tileimg_ofs_tile_h = 10U, /**< uint16 tile height.    */
  k_ra8_epub_tileimg_ofs_bpp    = 12U, /**< uint8 bytes per pixel. */
} ra8_epub_tileimg_ofs_t;

/**
 * @struct ra8_epub_tileimg_info_t
 * @brief Parsed geometry of a tile-atlas image entry.
 *
 * @details Filled by `ra8_epub_tile_binder_add()` from the on-disk header and
 *          exposed via `ra8_epub_tile_binder_info()` so a renderer can size the
 *          tile grid. `tile_cols`/`tile_rows` are the ceil-division tile counts.
 *
 * @since 0.1.0
 */
typedef struct {
  uint16_t width;     /**< Image width, pixels.               */
  uint16_t height;    /**< Image height, pixels.              */
  uint16_t tile_w;    /**< Tile width, pixels.                */
  uint16_t tile_h;    /**< Tile height, pixels.               */
  uint16_t tile_cols; /**< Ceil(width / tile_w) tile columns. */
  uint16_t tile_rows; /**< Ceil(height / tile_h) tile rows.   */
  uint8_t  bpp;       /**< Bytes per pixel (1..4).            */
} ra8_epub_tileimg_info_t;

/**
 * @struct ra8_epub_tile_source_t
 * @brief One registered tile-atlas image (private to the binder; treat as opaque).
 *
 * @invariant `book != NULL` and `info.bpp` in `[1, k_ra8_epub_tile_bpp_max]` while
 *            registered.
 * @since 0.1.0
 */
typedef struct {
  // cppcheck-suppress unusedStructMember
  ra8_epub_book_t* book; /**< Book whose archive holds the entry. */
  // cppcheck-suppress unusedStructMember
  char path[k_ra8_epub_max_path_len]; /**< Entry path (OPF-relative or rooted). */
  // cppcheck-suppress unusedStructMember
  uint32_t image_id; /**< Tile-cache key namespace for this image. */
  // cppcheck-suppress unusedStructMember
  ra8_epub_tileimg_info_t info; /**< Parsed geometry. */
} ra8_epub_tile_source_t;

/**
 * @struct ra8_epub_tile_binder_t
 * @brief Binds up to ::k_ra8_epub_tile_max_sources images to one tile cache.
 *
 * @details Owns a ::ra8_tile_cache whose decode-on-miss reads a tile off the
 *          registered source's entry via `ra8_epub_entry_pread`. Caller-owned;
 *          zero-initialise before `ra8_epub_tile_binder_init()`.
 *
 * @invariant Every valid `sources[i]` has a distinct `image_id`.
 * @since 0.1.0
 */
typedef struct {
  // cppcheck-suppress unusedStructMember
  ra8_tile_cache_t cache; /**< Owned tile cache (decode wired to the binder). */
  // cppcheck-suppress unusedStructMember
  ra8_epub_tile_source_t sources[k_ra8_epub_tile_max_sources]; /**< Registered images. */
  // cppcheck-suppress unusedStructMember
  uint8_t source_count; /**< Registered image count. */
} ra8_epub_tile_binder_t;

/**
 * @struct ra8_epub_img_loader_t
 * @brief Context for the real reflow `<img>` loader (`ra8_epub_reflow_img_load`).
 *
 * @details Binds an open book plus a caller-owned scratch buffer; the scratch
 *          capacity is the hard RAM ceiling on a single decoded `<img>` resource.
 *
 * @invariant `book != NULL`, `scratch != NULL`, `scratch_cap > 0` while bound.
 * @since 0.1.0
 */
typedef struct {
  // cppcheck-suppress unusedStructMember
  ra8_epub_book_t* book; /**< Book to resolve `<img src>` hrefs against. */
  // cppcheck-suppress unusedStructMember
  uint8_t* scratch; /**< Caller-owned encoded-bytes scratch. */
  // cppcheck-suppress unusedStructMember
  size_t scratch_cap; /**< Scratch capacity (bounded RAM ceiling). */
} ra8_epub_img_loader_t;

/**
 * @brief Initialise a tile binder over caller-supplied tile-cache storage (#231).
 *
 * @details
 * Wires @p storage into an owned ::ra8_tile_cache whose decode-on-miss is this
 * module's tile reader. The @p storage `decode` / `decode_ctx` fields are ignored
 * (the binder sets them); all other fields (cell memory + geometry + key/dim/meta
 * arrays + hash buckets) are the caller's and must out-live the binder.
 *
 * @param[out] binder  Binder to populate (zero-initialised by the caller).
 * @param[in]  storage Tile-cache storage config; `decode`/`decode_ctx` unused.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok               Binder ready; no images registered yet.
 * @retval k_ra8_err_null_ptr     @p binder or @p storage (or a required array) NULL.
 * @retval k_ra8_err_invalid_size A zero cell/bucket geometry in @p storage.
 *
 * @pre @p binder points at zeroed storage.
 * @pre @p storage arrays cover their sizes and out-live the binder.
 * @post On success the cache is empty and no source is registered.
 * @post On failure @p binder is left unusable.
 * @note Not thread-safe.
 * @see ra8_epub_tile_binder_add()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epub_tile_binder_init(ra8_epub_tile_binder_t*     binder,
                                                  const ra8_tile_cache_cfg_t* storage);

/**
 * @brief Register a tile-atlas image entry under @p image_id (#231).
 *
 * @details
 * Reads + validates the 16-byte atlas header off @p path (a stored entry, via
 * `ra8_epub_entry_pread`) and records the image geometry. After this, tiles of the
 * image are fetched with `ra8_epub_tile_binder_get()` keyed by @p image_id.
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
 * @retval k_ra8_err_invalid_arg       @p image_id already registered.
 * @retval k_ra8_err_validation_failed Bad magic / zero geometry / unsupported bpp.
 * @retval other                       Propagated from `ra8_epub_entry_pread`.
 *
 * @pre @p binder came from `ra8_epub_tile_binder_init()`.
 * @pre `path` is longer than 0 and shorter than ::k_ra8_epub_max_path_len.
 * @post On success `source_count` grew by one and the geometry is queryable.
 * @post On any error the source table is unchanged.
 * @note Not thread-safe.
 * @see ra8_epub_tile_binder_get()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epub_tile_binder_add(ra8_epub_tile_binder_t* binder,
                                                 ra8_epub_book_t*        book,
                                                 const char*             path,
                                                 uint32_t                image_id);

/**
 * @brief Report a registered image's parsed geometry (#231).
 *
 * @param[in]  binder   Binder with @p image_id registered.
 * @param[in]  image_id Image to query.
 * @param[out] out_info Receives the geometry on success.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok            Geometry reported.
 * @retval k_ra8_err_null_ptr  @p binder or @p out_info is NULL.
 * @retval k_ra8_err_not_found @p image_id is not registered.
 *
 * @pre @p binder is initialised; @p out_info is writable.
 * @pre @p image_id was registered via `ra8_epub_tile_binder_add()`.
 * @post On success `*out_info` holds the image geometry.
 * @post On failure `*out_info` is unmodified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epub_tile_binder_info(const ra8_epub_tile_binder_t* binder,
                                                  uint32_t                      image_id,
                                                  ra8_epub_tileimg_info_t*      out_info);

/**
 * @brief Get (and pin) one decoded tile of a registered image (#231).
 *
 * @details
 * Builds the tile-cache key `(image_id, tile_x, tile_y)` and fetches it through
 * the owned cache. On a miss the tile's pixels are read off the atlas entry (a
 * handful of positioned reads into one cache cell); on a hit the cell is reused.
 * The returned pixels are tightly packed (`out_tile->width * bpp` bytes per row)
 * and stay valid until `ra8_epub_tile_binder_put()`. Edge tiles report their true
 * (smaller) width/height.
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
 * @retval k_ra8_err_no_mem         Every cell is pinned (cannot evict for the miss).
 * @retval k_ra8_err_validation_failed A tile read came up short (corrupt entry).
 *
 * @pre @p binder is initialised and @p image_id registered.
 * @pre The caller will `ra8_epub_tile_binder_put()` the returned tile.
 * @post On success the cell's pin count grew by one.
 * @post On any error no new pin is held.
 * @note Not thread-safe.
 * @see ra8_epub_tile_binder_put()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epub_tile_binder_get(ra8_epub_tile_binder_t* binder,
                                                 uint32_t                image_id,
                                                 uint16_t                tile_x,
                                                 uint16_t                tile_y,
                                                 ra8_tile_t*             out_tile);

/**
 * @brief Release one pin taken by `ra8_epub_tile_binder_get()` (#231).
 *
 * @param[in] binder Initialised binder.
 * @param[in] pixels The `pixels` pointer from a returned ::ra8_tile_t.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok              Pin released.
 * @retval k_ra8_err_null_ptr    @p binder or @p pixels is NULL.
 * @retval k_ra8_err_invalid_arg @p pixels is not a pinned cell of this binder.
 *
 * @pre @p pixels came from `ra8_epub_tile_binder_get()` on this binder.
 * @pre @p binder is initialised.
 * @post On success the cell's pin count dropped by one.
 * @post On any error no state changed.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epub_tile_binder_put(ra8_epub_tile_binder_t* binder,
                                                 const uint8_t*          pixels);

/**
 * @brief Real reflow `<img>` byte loader off an EPUB book (#231).
 *
 * @details
 * A `ra8_reflow_image_loader_fn` (matching signature) that resolves @p href to an
 * EPUB manifest resource and returns its encoded bytes in the bound scratch
 * (`ra8_epub_get_resource`, which streams from the reader on demand). The scratch
 * capacity is the RAM ceiling: an image that does not fit is reported unavailable
 * (`k_ra8_err_no_mem`), which the reflow engine treats as "skip / placeholder"
 * rather than exceeding the budget. Wire it into the engine with
 * `ra8_reflow_set_image_loader()`, passing a ::ra8_epub_img_loader_t as @p ctx.
 *
 * @param[in]  ctx       A ::ra8_epub_img_loader_t* (book + scratch).
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
 * @retval other               Propagated from `ra8_epub_get_resource`.
 *
 * @pre @p ctx binds a live book, a non-NULL scratch, and `scratch_cap > 0`.
 * @pre @p href holds @p href_len readable bytes.
 * @post On success `*out_bytes` / `*out_len` describe the scratch contents.
 * @post On any error `*out_len == 0` and the engine falls back to a placeholder.
 * @note Not thread-safe; the scratch is single-decode.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_epub_reflow_img_load(void*           ctx,
                                                 const char*     href,
                                                 uint32_t        href_len,
                                                 const uint8_t** out_bytes,
                                                 size_t*         out_len);

#ifdef __cplusplus
}
#endif
