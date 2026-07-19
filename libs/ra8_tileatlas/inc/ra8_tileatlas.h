/**
 * @file ra8_tileatlas.h
 * @brief JOF band-tile atlas: the display-native normalized image format
 *        (#231, shared with the longstrip scroll #289 and codec policy #290).
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * A huge raster (a manga page or longstrip slice whose *decoded* size exceeds
 * the ~10 MB SDRAM working set) cannot be decoded whole on this device, and
 * JPEG/PNG offer no random access. JOF -- the Jump-Offset Format, named for
 * the access pattern below -- solves both at *import time*: the
 * source image is transcoded once (`ra8_tileatlas_produce()`) into a grid of
 * **independently decodable tiles** plus a **per-tile byte index**, so any
 * tile pages in later with one bounded read + one bounded inflate. A reader
 * parses the fixed-size footer, jumps to the index, reads the wanted tile's
 * byte offset, and jumps straight there -- hence *jump-offset*: O(1) random
 * access at full resolution, never a downscale.
 *
 * One format serves three consumers:
 *   - **#231** full-resolution in-EPUB manga pages: 2-D tile grids paged
 *     through `ra8_tile_cache` via the `ra8_epub_img_tiles` binder.
 *   - **#289** longstrip band-scroll: a band-tile is simply a tile the full
 *     image width (`tile_w == width`, one tile column); the tile index THEN
 *     IS the band index (byte offset + length per band), giving O(1) seek to
 *     any scroll position.
 *   - **#290** normalized on-device representation: every source codec
 *     (JPEG/PNG/...) converges on this one well-understood format at import,
 *     so render time touches a single decode path.
 *
 * ## On-disk layout (JOF, all integers little-endian)
 *
 * ```
 * [ header  | 32 bytes                      ]  at offset 0
 * [ tile 0  | index[0].length bytes         ]  tile streams, back-to-back
 * [ ...                                     ]
 * [ tile N-1                                ]
 * [ index   | tile_count * 8 bytes          ]  at footer.index_offset
 * [ footer  | 16 bytes                      ]  last 16 bytes of the atlas
 * ```
 *
 * ### Header (32 bytes)
 *
 * | Offset | Size | Field                                              |
 * |--------|------|----------------------------------------------------|
 * | 0      | 4    | magic `"JOF1"`                                     |
 * | 4      | 2    | u16 image width, pixels (1..32768)                 |
 * | 6      | 2    | u16 image height, pixels (1..32768)                |
 * | 8      | 2    | u16 tile width, pixels (1..width cap)              |
 * | 10     | 2    | u16 tile height, pixels (1..height cap)            |
 * | 12     | 1    | u8 bytes per pixel: 1=gray8, 3=RGB888, 4=RGBA8888  |
 * | 13     | 1    | u8 tile codec: 0=raw, 1=raw DEFLATE (RFC 1951)     |
 * | 14     | 2    | u16 reserved, must be 0                            |
 * | 16     | 4    | u32 tile count, must equal tile_cols * tile_rows   |
 * | 20     | 12   | reserved, must be 0                                |
 *
 * ### Tile streams
 *
 * Tile `n` (`n = tile_y * tile_cols + tile_x`, row-major) occupies
 * `[index[n].offset, index[n].offset + index[n].length)`. Edge tiles carry
 * their true clamped dimensions; a tile's decoded payload is exactly
 * `tw * th * bpp` bytes of tightly packed row-major pixels, where
 * `tw = min(tile_w, width - tile_x * tile_w)` and likewise for `th`.
 *   - codec 0 (raw): the stream is the payload verbatim.
 *   - codec 1 (deflate): the stream is one standalone raw-DEFLATE stream
 *     (RFC 1951, no zlib/gzip wrapper -- `ra8_io_compress()` output) that
 *     inflates to exactly the payload. Each tile is a self-contained
 *     ("intra-coded") stream: no cross-tile state, so any tile decodes
 *     without touching any other.
 *
 * ### Index (`tile_count` entries of 8 bytes, at `footer.index_offset`)
 *
 * | Offset | Size | Field                                             |
 * |--------|------|---------------------------------------------------|
 * | 0      | 4    | u32 tile byte offset (absolute, from atlas byte 0)|
 * | 4      | 4    | u32 tile byte length                              |
 *
 * ### Footer (last 16 bytes)
 *
 * | Offset | Size | Field                                             |
 * |--------|------|---------------------------------------------------|
 * | 0      | 4    | u32 index_offset (absolute)                       |
 * | 4      | 4    | u32 tile count, must equal the header field       |
 * | 8      | 4    | u32 total atlas size in bytes (self-check)        |
 * | 12     | 4    | magic `"JOFE"`                                    |
 *
 * The index trails the tile streams so a producer can emit the whole atlas
 * through an append-only sink (SD file write, ZIP store) in one forward
 * pass; a reader locates the index via the footer. Atlases are capped at
 * 4 GiB (u32 offsets) and 65536 tiles.
 *
 * ## Why DEFLATE and not JPEG for the tile codec (#290 resolution)
 *
 * Re-encoding tiles as JPEG would stack a second lossy generation on the
 * source image, which the no-quality-loss rule forbids; DEFLATE is lossless,
 * already in-tree (miniz, reused via `ra8_io_compress`/`ra8_io_decompress`),
 * decodes a 64 KiB tile in bounded RAM with zero heap, and compresses
 * manga/longstrip line art well. Raw (codec 0) remains for zero-decode paging
 * of already-tiny atlases.
 *
 * ## Validation model
 *
 * Atlases arrive from untrusted EPUB content. `ra8_tileatlas_parse()` and
 * `ra8_tileatlas_read_tile()` are fail-closed: every geometry field, both
 * magics, the reserved bytes, the tile-count cross-checks, the index window
 * and every per-tile offset/length/decoded-size are validated before any
 * pixel is trusted.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @see @ref md_docs_2formats_2JOF -- the full JOF wire-format specification
 *      (rationale, algorithms, worked example, failure modes).
 * @see ra8_tileatlas_produce.h  Import-time transcode producer.
 * @see ra8_epub_img_tiles.h     EPUB tile-cache binder over this format.
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

/**
 * @enum ra8_tileatlas_layout_t
 * @brief Fixed byte sizes and offsets of the JOF on-disk structures.
 *
 * @details Byte offsets are relative to the region they index (header fields
 *          from atlas byte 0, footer fields from the footer start). See the
 *          file comment for the authoritative layout tables.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_tileatlas_hdr_bytes      = 32U, /**< Header length.                   */
  k_ra8_tileatlas_footer_bytes   = 16U, /**< Footer length.                   */
  k_ra8_tileatlas_index_entry    = 8U,  /**< Bytes per tile-index entry.      */
  k_ra8_tileatlas_magic_len      = 4U,  /**< Magic string length (both ends). */
  k_ra8_tileatlas_ofs_magic      = 0U,  /**< Header: magic "JOF1".            */
  k_ra8_tileatlas_ofs_width      = 4U,  /**< Header: u16 image width.         */
  k_ra8_tileatlas_ofs_height     = 6U,  /**< Header: u16 image height.        */
  k_ra8_tileatlas_ofs_tile_w     = 8U,  /**< Header: u16 tile width.          */
  k_ra8_tileatlas_ofs_tile_h     = 10U, /**< Header: u16 tile height.         */
  k_ra8_tileatlas_ofs_bpp        = 12U, /**< Header: u8 bytes per pixel.      */
  k_ra8_tileatlas_ofs_codec      = 13U, /**< Header: u8 tile codec.           */
  k_ra8_tileatlas_ofs_reserved   = 14U, /**< Header: first reserved byte.     */
  k_ra8_tileatlas_ofs_tile_count = 16U, /**< Header: u32 tile count.          */
  k_ra8_tileatlas_ofs_reserved2  = 20U, /**< Header: second reserved run.     */
  k_ra8_tileatlas_ftr_index_off  = 0U,  /**< Footer: u32 index offset.        */
  k_ra8_tileatlas_ftr_tile_count = 4U,  /**< Footer: u32 tile count.          */
  k_ra8_tileatlas_ftr_total_size = 8U,  /**< Footer: u32 total atlas size.    */
  k_ra8_tileatlas_ftr_magic      = 12U, /**< Footer: magic "JOFE".            */
  k_ra8_tileatlas_idx_ofs_offset = 0U,  /**< Index entry: u32 tile offset.    */
  k_ra8_tileatlas_idx_ofs_length = 4U,  /**< Index entry: u32 tile length.    */
} ra8_tileatlas_layout_t;

/**
 * @enum ra8_tileatlas_limits_t
 * @brief Hard caps every JOF atlas must satisfy (fail-closed validation).
 *
 * @details The dimension cap bounds every producer/reader loop (NASA Rule 2);
 *          the tile-count cap bounds the index (512 KiB at 8 bytes/entry).
 *          `k_ra8_tileatlas_bpp_max` is the largest legal bytes-per-pixel.
 *
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ra8_tileatlas_max_dim   = 32768U, /**< Max image width/height, pixels. */
  k_ra8_tileatlas_max_tiles = 65536U, /**< Max tiles per atlas.            */
  k_ra8_tileatlas_bpp_max   = 4U,     /**< Max bytes per pixel.            */
} ra8_tileatlas_limits_t;

/**
 * @enum ra8_tileatlas_codec_t
 * @brief Per-atlas tile codec selector (header byte 13).
 *
 * @details Every tile in one atlas shares one codec. Both codecs are
 *          intra-coded: a tile decodes with no reference to any other tile.
 *
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_ra8_tileatlas_codec_raw     = 0U, /**< Tile stream is the packed pixels.   */
  k_ra8_tileatlas_codec_deflate = 1U, /**< Tile stream is one raw-DEFLATE run. */
} ra8_tileatlas_codec_t;

/**
 * @struct ra8_tileatlas_info_t
 * @brief Parsed + validated geometry of one JOF atlas.
 *
 * @details Filled by `ra8_tileatlas_parse()`; callers treat it as read-only
 *          after that. `tile_cols`/`tile_rows` are the ceil-division grid
 *          dimensions; `index_off`/`total_size` locate the tile index.
 *
 * @invariant `tile_count == (uint32_t)tile_cols * tile_rows` and
 *            `index_off + tile_count * 8 + 16 == total_size` once parsed.
 * @see ra8_tileatlas_parse()
 * @since 0.1.0
 */
typedef struct {
  uint16_t width;      /**< Image width, pixels.                       */
  uint16_t height;     /**< Image height, pixels.                      */
  uint16_t tile_w;     /**< Tile width, pixels.                        */
  uint16_t tile_h;     /**< Tile height, pixels.                       */
  uint16_t tile_cols;  /**< Ceil(width / tile_w) tile columns.         */
  uint16_t tile_rows;  /**< Ceil(height / tile_h) tile rows.           */
  uint8_t  bpp;        /**< Bytes per pixel (1, 3 or 4).               */
  uint8_t  codec;      /**< ::ra8_tileatlas_codec_t member.            */
  uint32_t tile_count; /**< Total tiles (== cols * rows).              */
  uint32_t index_off;  /**< Absolute byte offset of the tile index.    */
  uint32_t total_size; /**< Whole atlas byte length (footer included). */
} ra8_tileatlas_info_t;

/**
 * @typedef ra8_tileatlas_pread_fn
 * @brief Positioned-read seam over an atlas backing store (DIP).
 *
 * @details The reader never assumes what holds the atlas bytes: a stored ZIP
 *          entry (`ra8_epub_entry_pread`), an SD file, or a RAM region all
 *          plug in behind this signature. A short read (`*got < len`) at the
 *          backing's tail is legal; reads past the end report `*got == 0`.
 *
 * @param[in]  ctx    Backing-specific context.
 * @param[in]  offset Byte offset into the atlas.
 * @param[out] buf    Destination buffer (`len` writable bytes).
 * @param[in]  len    Bytes requested.
 * @param[out] got    Bytes actually read.
 * @return k_ra8_ok on success (short reads included); any error aborts the
 *         caller's operation with that code.
 * @since 0.1.0
 */
typedef ra8_err_t (
  *ra8_tileatlas_pread_fn)(void* ctx, uint64_t offset, uint8_t* buf, size_t len, size_t* got);

/**
 * @struct ra8_tileatlas_memstore_t
 * @brief Append-only memory store: the simplest atlas backing (RAM/SDRAM).
 *
 * @details Serves both halves of the producer/reader contract: its sink
 *          appends produced bytes at `len`, its pread serves them back.
 *          Zero-initialise, then set `buf`/`cap`.
 *
 * @invariant `len <= cap` at all times.
 * @see ra8_tileatlas_memstore_sink()
 * @see ra8_tileatlas_memstore_pread()
 * @since 0.1.0
 */
typedef struct {
  uint8_t* buf; /**< Caller-owned backing bytes.       */
  size_t   cap; /**< Capacity of `buf`.                */
  size_t   len; /**< Bytes appended so far (see sink). */
} ra8_tileatlas_memstore_t;

/**
 * @brief Append `len` bytes to a ::ra8_tileatlas_memstore_t (producer sink).
 *
 * @details Matching `ra8_tileatlas_sink_fn` shape (see the producer header):
 *          bind with `ctx = &store`. Fails closed when the store is full so a
 *          hostile source can never write past `cap`.
 *
 * @param[in] ctx Store to append to (a ::ra8_tileatlas_memstore_t*).
 * @param[in] buf Bytes to append.
 * @param[in] len Byte count (0 is a no-op).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Bytes appended; `store->len` advanced.
 * @retval k_ra8_err_null_ptr @p ctx, `store->buf`, or @p buf is NULL.
 * @retval k_ra8_err_no_mem   The append would exceed `store->cap`.
 *
 * @pre @p ctx points at an initialised store (buf/cap set).
 * @pre @p buf holds @p len readable bytes.
 * @post On success exactly @p len bytes were copied at the old `len`.
 * @post On any error the store is unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_tileatlas_memstore_sink(void* ctx, const uint8_t* buf, size_t len);

/**
 * @brief Positioned read from a ::ra8_tileatlas_memstore_t (reader seam).
 *
 * @details Matching ::ra8_tileatlas_pread_fn: bind with `ctx = &store`.
 *          Reads clamp at `store->len`; a read at/after the end reports
 *          `*got == 0` without error, mirroring `ra8_epub_entry_pread`.
 *
 * @param[in]  ctx    Store to read from (a ::ra8_tileatlas_memstore_t*).
 * @param[in]  offset Byte offset into the stored atlas.
 * @param[out] buf    Destination buffer.
 * @param[in]  len    Bytes requested.
 * @param[out] got    Bytes actually copied (possibly short at the tail).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok           Window read (short reads included).
 * @retval k_ra8_err_null_ptr @p ctx, `store->buf`, @p buf, or @p got is NULL.
 *
 * @pre @p ctx points at a store previously filled through the sink.
 * @pre @p buf holds @p len writable bytes.
 * @post `*got <= len` bytes were copied into @p buf.
 * @post On any error `*got == 0`.
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_tileatlas_memstore_pread(void* ctx, uint64_t offset, uint8_t* buf, size_t len, size_t* got);

/**
 * @brief Parse + validate a JOF atlas's header, footer and index bounds.
 *
 * @details
 * Reads the 32-byte header at offset 0 and the 16-byte footer at
 * `total_size - 16`, then cross-checks every field fail-closed: both magics,
 * non-zero geometry within ::ra8_tileatlas_limits_t, legal bpp/codec, zeroed
 * reserved bytes, `tile_count == cols * rows` in both header and footer, the
 * footer's `total_size` against the caller-supplied backing size, and the
 * index window `[index_off, index_off + 8 * tile_count + 16] == total_size`.
 * Per-tile offset/length windows are validated later, per read, by
 * `ra8_tileatlas_read_tile()` (the index itself may be larger than any
 * bounded parse buffer).
 *
 * @param[in]  pread      Backing read seam (non-NULL).
 * @param[in]  pread_ctx  Context for @p pread.
 * @param[in]  total_size Backing size in bytes (entry/file/store length).
 * @param[out] out_info   Receives the validated geometry.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                    Atlas structurally valid; info filled.
 * @retval k_ra8_err_null_ptr          @p pread or @p out_info is NULL.
 * @retval k_ra8_err_invalid_size      @p total_size cannot hold header+footer
 *                                     or exceeds the u32 format cap.
 * @retval k_ra8_err_validation_failed A magic/geometry/cross-check failed.
 * @retval other                       Propagated from @p pread.
 *
 * @pre @p pread serves the atlas bytes for `[0, total_size)`.
 * @pre @p out_info is writable.
 * @post On success `*out_info` satisfies the documented invariants.
 * @post On any error `*out_info` is unspecified and must not be used.
 * @note Thread-safe for distinct outputs (no shared state).
 * @see ra8_tileatlas_read_tile()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_tileatlas_parse(ra8_tileatlas_pread_fn pread,
                                            void*                  pread_ctx,
                                            uint64_t               total_size,
                                            ra8_tileatlas_info_t*  out_info);

/**
 * @brief Report the true (edge-clamped) pixel dimensions of one tile.
 *
 * @param[in]  info  Parsed atlas geometry.
 * @param[in]  tile_x Tile column, `[0, tile_cols)`.
 * @param[in]  tile_y Tile row, `[0, tile_rows)`.
 * @param[out] out_w Receives the tile's width, pixels.
 * @param[out] out_h Receives the tile's height, pixels.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok               Dimensions reported.
 * @retval k_ra8_err_null_ptr     @p info, @p out_w, or @p out_h is NULL.
 * @retval k_ra8_err_out_of_range @p tile_x / @p tile_y outside the grid.
 *
 * @pre @p info came from a successful `ra8_tileatlas_parse()`.
 * @pre @p out_w and @p out_h are writable.
 * @post On success both outputs are in `[1, tile_w]` / `[1, tile_h]`.
 * @post On any error neither output is modified.
 * @note Thread-safe (pure over its inputs).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_tileatlas_tile_dims(const ra8_tileatlas_info_t* info,
                                                uint16_t                    tile_x,
                                                uint16_t                    tile_y,
                                                uint16_t*                   out_w,
                                                uint16_t*                   out_h);

/**
 * @brief Worst-case stored-tile byte bound for scratch/cell sizing.
 *
 * @details For raw tiles the stored stream equals the payload; for deflate
 *          the stored stream is bounded by the incompressible-input expansion
 *          `raw + raw/8 + 256` (a safe over-estimate of the miniz raw-DEFLATE
 *          worst case). Size `ra8_tileatlas_read_tile()`'s scratch with this
 *          over `raw = tile_w * tile_h * bpp`.
 *
 * @param[in] raw_bytes Decoded tile payload size in bytes.
 *
 * @return Upper bound on the stored tile stream length, bytes.
 * @retval >=raw_bytes Always at least the payload size plus margin.
 *
 * @pre @p raw_bytes is a real tile payload size (fits uint32_t with margin).
 * @pre The atlas codec is one of ::ra8_tileatlas_codec_t.
 * @post No state mutated.
 * @post Return >= @p raw_bytes + 256.
 * @note Thread-safe (pure).
 * @since 0.1.0
 */
[[nodiscard]] uint32_t ra8_tileatlas_stored_bound(uint32_t raw_bytes);

/**
 * @brief Read + decode one tile into caller pixels, in bounded RAM.
 *
 * @details
 * Fetches the tile's index entry (one 8-byte pread), validates its window
 * against the tile-stream region, reads the stored stream, and decodes it:
 * raw tiles are pread directly into @p out_px; deflate tiles are pread into
 * @p scratch and inflated with `ra8_io_decompress()` (zero heap). The decoded
 * byte count must equal the tile's exact payload size or the read fails
 * closed. Resident cost is `scratch_cap + out_cap`, independent of the image
 * size -- the property #231 needs for decode-on-demand paging.
 *
 * @param[in]  pread       Backing read seam (non-NULL).
 * @param[in]  pread_ctx   Context for @p pread.
 * @param[in]  info        Parsed atlas geometry.
 * @param[in]  tile_x      Tile column, `[0, tile_cols)`.
 * @param[in]  tile_y      Tile row, `[0, tile_rows)`.
 * @param[out] scratch     Stored-stream staging (deflate codec only; may be
 *                         NULL for raw atlases).
 * @param[in]  scratch_cap Capacity of @p scratch; must cover
 *                         `ra8_tileatlas_stored_bound()` of the tile payload
 *                         for deflate atlases.
 * @param[out] out_px      Destination pixel buffer.
 * @param[in]  out_cap     Capacity of @p out_px; must cover the payload.
 * @param[out] out_w       Receives the tile's clamped width, pixels.
 * @param[out] out_h       Receives the tile's clamped height, pixels.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                    Tile decoded into @p out_px.
 * @retval k_ra8_err_null_ptr          A required pointer is NULL.
 * @retval k_ra8_err_out_of_range      @p tile_x / @p tile_y outside the grid.
 * @retval k_ra8_err_invalid_size      @p out_cap (or @p scratch_cap) too small
 *                                     for this tile.
 * @retval k_ra8_err_validation_failed Index/stream window corrupt, short
 *                                     read, or decoded size mismatch.
 * @retval other                       Propagated from @p pread.
 *
 * @pre @p info came from a successful `ra8_tileatlas_parse()` over the same
 *      backing.
 * @pre @p out_px holds @p out_cap writable bytes.
 * @post On success exactly `(*out_w) * (*out_h) * bpp` bytes are valid.
 * @post On any error @p out_px content is unspecified.
 * @note Thread-safe for distinct buffers (no shared state).
 * @see ra8_tileatlas_parse()
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_tileatlas_read_tile(ra8_tileatlas_pread_fn      pread,
                                                void*                       pread_ctx,
                                                const ra8_tileatlas_info_t* info,
                                                uint16_t                    tile_x,
                                                uint16_t                    tile_y,
                                                uint8_t*                    scratch,
                                                uint32_t                    scratch_cap,
                                                uint8_t*                    out_px,
                                                uint32_t                    out_cap,
                                                uint16_t*                   out_w,
                                                uint16_t*                   out_h);

#ifdef __cplusplus
}
#endif
