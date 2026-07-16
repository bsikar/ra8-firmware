/**
 * @file ra8_tileatlas_produce.h
 * @brief Import-time transcode producer: JPEG/PNG -> RTA1 band-tile atlas in
 *        bounded RAM (#231).
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * `ra8_tileatlas_produce()` turns an arbitrary encoded source image into the
 * RTA1 atlas (`ra8_tileatlas.h`) in **one forward pass** with a **constant
 * RAM high-water**: the source arrives through a pull callback (never held
 * whole), the decoder emits pixel rows in bounded stripes (never the whole
 * frame), a band accumulator gathers one tile row at a time, each tile is
 * cut + intra-coded + appended to the sink, and the index/footer trail the
 * data so the sink can be append-only (an SD file, a memstore over SDRAM).
 * A page whose decoded size exceeds SDRAM transcodes without ever being
 * resident -- the memory ceiling is the caller's fixed work arena plus the
 * caller's buffers, independent of the image.
 *
 * Resident working set (all carved from the caller's single `work` arena;
 * `ra8_tileatlas_work_bytes()` computes the exact requirement):
 *   - deflate scratch (one `tdefl_compressor`, codec 1 only),
 *   - the decoder set (JPEG: 128 KiB input window + one MCU-row stripe;
 *     PNG: inflate state + 64 KiB ring + three row buffers),
 *   - one band (`width * tile_h * bpp`), one tile stage, one compressed
 *     tile bound, and the tile index (8 bytes per tile).
 *
 * Sources: baseline JPEG (grayscale -> 1 bpp, colour -> 3 bpp RGB888) and
 * PNG (8-bit gray -> 1 bpp; RGB / palette -> 3 bpp; gray+alpha / RGBA /
 * palette+tRNS -> 4 bpp). Anything else -- progressive JPEG, 16-bit or
 * interlaced PNG, other formats -- is rejected fail-closed
 * (`k_ra8_err_not_supported`); the caller falls back to the whole-decode
 * path for images small enough to afford it. **No downscaling, ever**:
 * output pixels are the decoded pixels, full resolution, lossless.
 *
 * This is untrusted EPUB content: every source field is bounds-checked, all
 * loops are capped, and any structural anomaly aborts the transcode with an
 * error rather than a partial atlas being trusted. Zero heap: the producer
 * allocates nothing -- every byte of state lives in caller buffers.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @see ra8_tileatlas.h  The output format + reader.
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_tileatlas.h"

/**
 * @typedef ra8_tileatlas_pull_fn
 * @brief Forward byte source for the producer (DIP seam).
 *
 * @details Strictly sequential, single pass: each call appends the next
 *          bytes of the encoded source image. `*got == 0` signals a clean
 *          end of stream; any error return aborts the transcode with that
 *          code. An EPUB entry cursor (`ra8_epub_entry_read`) matches this
 *          shape directly.
 *
 * @param[in]  ctx Source-specific context.
 * @param[out] buf Destination buffer (`cap` writable bytes).
 * @param[in]  cap Capacity of @p buf.
 * @param[out] got Bytes delivered this call (0 = end of stream).
 * @return k_ra8_ok on success; any error aborts the transcode.
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_tileatlas_pull_fn)(void* ctx, uint8_t* buf, size_t cap, size_t* got);

/**
 * @typedef ra8_tileatlas_sink_fn
 * @brief Append-only byte sink for the produced atlas (DIP seam).
 *
 * @details Bytes arrive strictly in atlas order (header, tiles, index,
 *          footer); the sink never seeks. `ra8_tileatlas_memstore_sink()`
 *          is the RAM-backed reference implementation; an `ra8_fs` file
 *          writer wraps `ra8_fs_write` identically. Any error return
 *          aborts the transcode with that code.
 *
 * @param[in] ctx Sink-specific context.
 * @param[in] buf Bytes to append.
 * @param[in] len Byte count.
 * @return k_ra8_ok on success; any error aborts the transcode.
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_tileatlas_sink_fn)(void* ctx, const uint8_t* buf, size_t len);

/**
 * @struct ra8_tileatlas_produce_cfg_t
 * @brief Producer configuration: source, sink, tile geometry and work arena.
 *
 * @details `max_width`/`max_height` are the caller's fail-closed budget caps
 *          (0 selects the format cap ::k_ra8_tileatlas_max_dim); a source
 *          exceeding them aborts with `k_ra8_err_invalid_size` before any
 *          pixel decodes. The producer carves every internal buffer from
 *          `work`; size it with `ra8_tileatlas_work_bytes()` over the same
 *          caps. The arena is the RAM high-water by construction.
 *
 * @invariant `tile_w`/`tile_h` are non-zero and `work` covers `work_cap`.
 * @see ra8_tileatlas_work_bytes()
 * @since 0.1.0
 */
typedef struct {
  ra8_tileatlas_pull_fn pull;       /**< Encoded-source byte stream.                  */
  void*                 pull_ctx;   /**< Context for `pull`.                          */
  ra8_tileatlas_sink_fn sink;       /**< Atlas byte sink (append-only).               */
  void*                 sink_ctx;   /**< Context for `sink`.                          */
  uint16_t              tile_w;     /**< Tile width, pixels (>= 1).                   */
  uint16_t              tile_h;     /**< Tile height, pixels (>= 1).                  */
  uint8_t               codec;      /**< ::ra8_tileatlas_codec_t member.              */
  uint16_t              max_width;  /**< Width budget cap (0 = format cap).           */
  uint16_t              max_height; /**< Height budget cap (0 = format cap).          */
  uint8_t*              work;       /**< Single working arena (all state lives here). */
  size_t                work_cap;   /**< Arena size; see ra8_tileatlas_work_bytes().  */
} ra8_tileatlas_produce_cfg_t;

/**
 * @brief Compute the work-arena size the producer needs for given caps.
 *
 * @details
 * Sums the worst of the two decoder carve sets (JPEG window + stripe vs PNG
 * inflate state + ring + rows) with the band, tile stage, compressed-tile
 * bound, deflate scratch and tile index for the given budget caps, plus
 * per-carve alignment slack. Passing the same caps here and in the config
 * guarantees `ra8_tileatlas_produce()` never fails on arena exhaustion for
 * an in-budget source.
 *
 * @param[in] max_width  Largest source width to support (>= 1, <= cap).
 * @param[in] max_height Largest source height to support (>= 1, <= cap).
 * @param[in] tile_w     Tile width the producer will use (>= 1).
 * @param[in] tile_h     Tile height the producer will use (>= 1).
 *
 * @return Required arena size in bytes, or 0 on nonsense inputs.
 * @retval 0   An argument was zero or exceeded ::k_ra8_tileatlas_max_dim,
 *             or the tile grid would exceed ::k_ra8_tileatlas_max_tiles.
 * @retval >0  Byte size to allocate for `work`.
 *
 * @pre Arguments describe the caller's real budget caps.
 * @pre The same `tile_w`/`tile_h` will be used in the produce config.
 * @post No state mutated.
 * @post A `work_cap` of the returned size never exhausts mid-transcode.
 * @note Thread-safe (pure).
 * @see ra8_tileatlas_produce()
 * @since 0.1.0
 */
[[nodiscard]] uint32_t
ra8_tileatlas_work_bytes(uint16_t max_width, uint16_t max_height, uint16_t tile_w, uint16_t tile_h);

/**
 * @brief Transcode one encoded JPEG/PNG source into an RTA1 atlas (#231).
 *
 * @details
 * Sniffs the source magic, streams it through the matching bounded stripe
 * decoder, accumulates rows into one band, cuts + encodes each tile through
 * the configured codec, and appends header / tiles / index / footer to the
 * sink in one forward pass. See the file comment for the resident-set
 * breakdown. On success `out_info` describes the finished atlas exactly as
 * `ra8_tileatlas_parse()` would report it.
 *
 * The whole decoded image is never resident: every internal buffer -- the
 * decoder window/stripe set, the band, the tile stage, the compressor state
 * and the index -- is carved from `work`, so the transcode's RAM high-water
 * is exactly `work_cap` bytes of working state, independent of the image.
 *
 * @param[in]  cfg      Producer configuration (see the struct contract).
 * @param[out] out_info Receives the finished atlas geometry.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                    Atlas fully written to the sink.
 * @retval k_ra8_err_null_ptr          A required pointer in @p cfg is NULL.
 * @retval k_ra8_err_invalid_arg       Zero tile geometry or unknown codec.
 * @retval k_ra8_err_invalid_size      Source exceeds the budget caps, the
 *                                     grid exceeds the tile cap, or `work`
 *                                     is too small (fail-closed).
 * @retval k_ra8_err_not_supported     Source format not JPEG/PNG, or an
 *                                     unsupported variant (progressive,
 *                                     interlaced, 16-bit, ...).
 * @retval k_ra8_err_protocol_error    Malformed / truncated / hostile
 *                                     source structure.
 * @retval k_ra8_err_validation_failed Pixel-stream inconsistency (row
 *                                     count, inflate size, palette index).
 * @retval other                       Propagated from pull / sink.
 *
 * @pre @p cfg->work covers `work_cap` bytes sized per
 *      `ra8_tileatlas_work_bytes()`.
 * @pre @p cfg->pull delivers the encoded source strictly in order, once.
 * @post On success the sink holds one complete, parseable RTA1 atlas.
 * @post On any error the sink holds a partial atlas that must be discarded
 *       (it will fail `ra8_tileatlas_parse()` -- no torn atlas is readable).
 * @note Not thread-safe (module-static decoder contexts).
 *
 * @par Example:
 * @code
 * ra8_tileatlas_memstore_t store = { .buf = sdram_buf, .cap = sizeof sdram_buf };
 * ra8_tileatlas_produce_cfg_t cfg = {
 *   .pull = epub_entry_pull, .pull_ctx = &cursor,
 *   .sink = ra8_tileatlas_memstore_sink, .sink_ctx = &store,
 *   .tile_w = 256, .tile_h = 256,
 *   .codec = k_ra8_tileatlas_codec_deflate,
 *   .max_width = 8192, .max_height = 16384,
 *   .work = arena, .work_cap = sizeof arena,
 * };
 * ra8_tileatlas_info_t info;
 * ra8_err_t err = ra8_tileatlas_produce(&cfg, &info);
 * @endcode
 *
 * @see ra8_tileatlas_parse()      Validate / reopen the produced atlas.
 * @see ra8_tileatlas_read_tile()  Page tiles back in bounded RAM.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_tileatlas_produce(const ra8_tileatlas_produce_cfg_t* cfg,
                                              ra8_tileatlas_info_t*              out_info);

#ifdef __cplusplus
}
#endif
