/**
 * @file ra8_tileatlas_internal.h
 * @brief Module-private seams shared by the producer translation units.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * The producer is split across translation units to keep each under the
 * maintainability line cap:
 *
 *   - `ra8_tileatlas_produce.c`      -- sniff/dispatch, band accumulator,
 *     tile cut + encode + sink, index/footer emission, JPEG/PNG arms.
 *   - `ra8_tileatlas_produce_webp.c` -- the whole-frame WebP arm (#290):
 *     `ra8_tileatlas_webp_work_bytes()` and `ra8_ta_priv_webp_transcode()`.
 *   - `ra8_tileatlas_png.c`          -- the streaming PNG scanline decoder.
 *
 * This header carries the bump allocator over the caller's work arena, the
 * geometry/rows callback contracts the PNG decoder reports through (the JPEG
 * path reports through `ra8_jpeg_sw_decode_stripes()`'s own public seams
 * instead), and the producer-state / prefix-pull seams the WebP arm shares
 * with the dispatcher.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_tileatlas_produce.h"

/**
 * @enum ra8_ta_bump_const_t
 * @brief Bump-allocator alignment constants.
 */
typedef enum : uint8_t {
  k_ra8_ta_bump_align = 8U, /**< Every carve is 8-byte aligned. */
} ra8_ta_bump_const_t;

/**
 * @struct ra8_ta_bump_t
 * @brief Linear (bump) allocator over the caller's work arena.
 *
 * @details Carves 8-byte-aligned regions front to back; exhaustion is the
 *          producer's fail-closed "source too large for the configured
 *          budget" signal. Never frees -- the arena resets per transcode.
 *
 * @invariant `off <= cap` at all times.
 * @since 0.1.0
 */
typedef struct {
  uint8_t* base; /**< Arena base (caller's work buffer). */
  size_t   cap;  /**< Arena capacity, bytes.             */
  size_t   off;  /**< First free byte.                   */
} ra8_ta_bump_t;

/**
 * @brief Carve @p len 8-byte-aligned bytes from the bump arena.
 *
 * @param[in,out] bump Arena state (offset advances).
 * @param[in]     len  Bytes requested.
 *
 * @return Pointer to the carved region, or NULL on exhaustion.
 * @retval NULL     The arena cannot fit @p len more aligned bytes.
 * @retval non-NULL 8-byte-aligned region of @p len bytes (assignable to any
 *                  object pointer whose alignment is at most 8).
 *
 * @pre @p bump is non-NULL with `base` covering `cap` bytes.
 * @pre @p len is a real buffer size (> 0).
 * @post On success `bump->off` advanced past the carve.
 * @post On NULL the arena is unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV void* ra8_ta_priv_bump_take(ra8_ta_bump_t* bump, size_t len);

/**
 * @typedef ra8_ta_geom_fn
 * @brief Producer geometry hook: fires once when the source dimensions and
 *        output channel count are known, before any pixel row is emitted.
 *
 * @param[in] ctx      Producer context.
 * @param[in] width    Source width, pixels (>= 1).
 * @param[in] height   Source height, pixels (>= 1).
 * @param[in] channels Output bytes per pixel (1, 3 or 4).
 * @return k_ra8_ok to continue; any error aborts the decode with that code.
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_ta_geom_fn)(void* ctx, uint16_t width, uint16_t height, uint8_t channels);

/**
 * @typedef ra8_ta_rows_fn
 * @brief Producer row sink: receives decoded pixel rows strictly in order.
 *
 * @param[in] ctx      Producer context.
 * @param[in] px       Row pixels (`nrows * width * channels` bytes, packed).
 * @param[in] width    Row width, pixels.
 * @param[in] y0       Image row of the first delivered row.
 * @param[in] nrows    Rows delivered this call (>= 1).
 * @param[in] channels Bytes per pixel (matches the geometry hook).
 * @return k_ra8_ok to continue; any error aborts the decode with that code.
 * @since 0.1.0
 */
typedef ra8_err_t (*ra8_ta_rows_fn)(void*          ctx,
                                    const uint8_t* px,
                                    uint16_t       width,
                                    uint16_t       y0,
                                    uint16_t       nrows,
                                    uint8_t        channels);

/**
 * @brief Streaming PNG scanline decode: pull bytes in, emit rows in order.
 *
 * @details
 * Bounded-RAM PNG decoder for the transcode producer: 8-bit depth, colour
 * types 0/2/3/4/6, non-interlaced. IDAT inflates through miniz `tinfl` into
 * a 64 KiB ring, scanlines are unfiltered against one previous row and
 * translated to the output layout (gray -> 1, RGB / opaque palette -> 3,
 * gray+alpha / RGBA / palette+tRNS -> 4), then handed to @p on_rows one row
 * at a time. All working buffers are carved from @p bump. Interlaced,
 * 16-bit, and out-of-spec structures are rejected fail-closed. Chunk CRCs
 * are not verified (the ZIP layer above already integrity-checks the entry;
 * the zlib Adler32 inside IDAT is verified).
 *
 * @param[in]     pull     Sequential byte source positioned at byte 0 of the
 *                         PNG stream (the producer replays sniffed bytes).
 * @param[in]     pull_ctx Context for @p pull.
 * @param[in,out] bump     Work-arena allocator for the decoder's buffers.
 * @param[in]     max_w    Fail-closed width cap, pixels.
 * @param[in]     max_h    Fail-closed height cap, pixels.
 * @param[in]     on_geom  Geometry hook (fires once, before rows).
 * @param[in]     on_rows  Row sink (fires `height` times, in order).
 * @param[in]     cb_ctx   Context for both hooks.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                    Every row emitted.
 * @retval k_ra8_err_invalid_size      Dimensions exceed the caps or the
 *                                     arena is exhausted.
 * @retval k_ra8_err_not_supported     Interlaced / 16-bit / unknown colour
 *                                     type / non-zero compression or filter
 *                                     method.
 * @retval k_ra8_err_protocol_error    Malformed chunk structure or a
 *                                     corrupt / truncated deflate stream.
 * @retval k_ra8_err_validation_failed Pixel-stream inconsistency (filter
 *                                     byte, palette index, row count).
 * @retval other                       Propagated from pull / the hooks.
 *
 * @pre @p pull delivers the PNG from its first signature byte.
 * @pre @p bump has capacity per `ra8_tileatlas_work_bytes()`.
 * @post On success exactly `height` rows were emitted, in order.
 * @post On any error emission stops; the transcode aborts.
 * @note Not thread-safe (module-static inflate context).
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_ta_priv_png_rows(ra8_tileatlas_pull_fn pull,
                                        void*                 pull_ctx,
                                        ra8_ta_bump_t*        bump,
                                        uint16_t              max_w,
                                        uint16_t              max_h,
                                        ra8_ta_geom_fn        on_geom,
                                        ra8_ta_rows_fn        on_rows,
                                        void*                 cb_ctx);

/**
 * @enum ra8_ta_carve_const_t
 * @brief Carve-sizing constant shared by both work-arena calculators.
 */
typedef enum : uint32_t {
  k_ra8_ta_carve_slack = 128U, /**< Alignment slack folded into every arena sizing. */
} ra8_ta_carve_const_t;

/**
 * @struct ra8_ta_prefix_pull_t
 * @brief Pull adapter that replays the sniffed head bytes, then delegates.
 *
 * @details The producer must peek the source magic before choosing a decoder,
 *          but every decoder expects the stream from byte 0; this adapter
 *          serves the peeked bytes first, then transparently delegates to the
 *          inner source. Shared so the WebP arm can pull its whole compressed
 *          source through the same replay as the streaming decoders.
 *
 * @invariant `pos <= head_len` at all times.
 * @since 0.1.0
 */
typedef struct {
  const uint8_t*        head;      /**< Sniffed bytes to replay. */
  size_t                head_len;  /**< Sniffed byte count.      */
  size_t                pos;       /**< Replay cursor.           */
  ra8_tileatlas_pull_fn inner;     /**< Real source.             */
  void*                 inner_ctx; /**< Context for the source.  */
} ra8_ta_prefix_pull_t;

/**
 * @struct ra8_ta_prod_state_t
 * @brief Whole transcode state (module-static instance in the producer TU).
 *
 * @details One static instance mirrors the decoder pattern; every sized buffer
 *          inside is a bump carve out of the caller's arena. Shared with the
 *          WebP arm, which binds geometry and feeds rows through the same
 *          state so its atlas bytes are identical to the streaming path.
 *
 * @invariant `band_fill <= tile_h` and `tiles_done <= tile_count`.
 * @since 0.1.0
 */
typedef struct {
  const ra8_tileatlas_produce_cfg_t* cfg;        /**< Caller configuration.    */
  ra8_ta_bump_t                      bump_store; /**< Arena allocator state.   */
  ra8_ta_bump_t*                     bump;       /**< Arena allocator (owned). */

  uint16_t cap_w; /**< Effective width cap.  */
  uint16_t cap_h; /**< Effective height cap. */

  uint16_t w;          /**< Source width, pixels.      */
  uint16_t h;          /**< Source height, pixels.     */
  uint8_t  bpp;        /**< Output bytes per pixel.    */
  uint16_t tile_cols;  /**< Tile grid columns.         */
  uint16_t tile_rows;  /**< Tile grid rows.            */
  uint32_t tile_count; /**< Total tiles (cols * rows). */
  uint8_t  geom_done;  /**< 1 once geometry was bound. */

  uint8_t* band;    /**< One tile row of decoded pixels.   */
  uint8_t* stage;   /**< One packed tile payload.          */
  uint8_t* cmp;     /**< One encoded tile (deflate codec). */
  uint32_t cmp_cap; /**< Capacity of `cmp`.                */
  uint8_t* idx;     /**< Tile index bytes (8 per tile).    */
  uint8_t* dfl;     /**< Deflate compressor scratch.       */
  uint32_t dfl_len; /**< Scratch length (0 for raw codec). */

  uint32_t rows_seen;  /**< Decoded rows accumulated so far. */
  uint32_t band_fill;  /**< Rows currently in the band.      */
  uint32_t tiles_done; /**< Tiles encoded + recorded so far. */
  uint32_t written;    /**< Atlas bytes sunk so far.         */
} ra8_ta_prod_state_t;

/**
 * @brief Pull adapter: replay the sniffed head, then delegate to the source.
 *
 * @details Serves the replay bytes first, then transparently delegates to the
 *          inner source. Matches ::ra8_tileatlas_pull_fn so it can be passed
 *          as the pull seam to any decoder.
 *
 * @param[in]  ctx A ::ra8_ta_prefix_pull_t.
 * @param[out] buf Destination buffer.
 * @param[in]  cap Capacity of @p buf.
 * @param[out] got Bytes delivered.
 *
 * @return Result code.
 * @retval k_ra8_ok Delivered replay or source bytes.
 * @retval other    Propagated from the inner source.
 *
 * @pre @p ctx points at an initialised adapter.
 * @pre @p buf holds @p cap writable bytes.
 * @post `*got <= cap` bytes were written to @p buf.
 * @post The replay cursor never exceeds the head length.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_ta_priv_prefix_pull(void* ctx, uint8_t* buf, size_t cap, size_t* got);

/**
 * @brief Bind the source geometry: validate caps, carve buffers, emit header.
 *
 * @details Fires once per transcode (from any decoder arm). Rejects, fail
 *          closed: dimensions above the caps, a tile grid above the format
 *          cap, and any carve the arena cannot fit. On success the 32-byte
 *          JOF header has been sunk. Matches ::ra8_ta_geom_fn.
 *
 * @param[in] ctx      The producer state (::ra8_ta_prod_state_t).
 * @param[in] width    Source width, pixels.
 * @param[in] height   Source height, pixels.
 * @param[in] channels Output bytes per pixel (1, 3 or 4).
 *
 * @return Result code.
 * @retval k_ra8_ok                Geometry bound; header written.
 * @retval k_ra8_err_invalid_size  Over the caps / grid cap / arena exhausted.
 * @retval k_ra8_err_invalid_state The geometry hook fired twice.
 * @retval other                   Propagated from the sink.
 *
 * @pre The decoder validated @p width/@p height non-zero.
 * @pre `st->cfg` and `st->bump` are bound.
 * @post On success the pixel-path buffers are carved and the header sunk.
 * @post On error the transcode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_ta_priv_on_geom(void*    ctx,
                                       uint16_t width,
                                       uint16_t height,
                                       uint8_t  channels);

/**
 * @brief Row sink shared by every decoder arm: accumulate, flush full bands.
 *
 * @details Enforces the strict in-order row contract (`y0 == rows_seen`), then
 *          copies the delivered rows into the band, flushing a tile row every
 *          time the band fills. A row group may span a band boundary; the copy
 *          loop splits it. Matches ::ra8_ta_rows_fn.
 *
 * @param[in] ctx      The producer state (::ra8_ta_prod_state_t).
 * @param[in] px       Decoded row pixels.
 * @param[in] width    Row width, pixels.
 * @param[in] y0       Image row of the first delivered row.
 * @param[in] nrows    Rows delivered.
 * @param[in] channels Bytes per pixel.
 *
 * @return Result code.
 * @retval k_ra8_ok                    Rows accumulated (bands maybe flushed).
 * @retval k_ra8_err_validation_failed Contract violation (order, geometry).
 * @retval other                       Propagated from the flush stage.
 *
 * @pre The geometry hook has fired (`geom_done == 1`).
 * @pre @p px holds `nrows * width * channels` bytes.
 * @post `rows_seen` advanced by @p nrows on success.
 * @post On error the transcode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_ta_priv_on_rows(void*          ctx,
                                       const uint8_t* px,
                                       uint16_t       width,
                                       uint16_t       y0,
                                       uint16_t       nrows,
                                       uint8_t        channels);

/**
 * @brief Transcode a WebP source into the JOF tile path (whole-frame, #290).
 *
 * @details Pulls the whole compressed source into `cfg->webp_work`, reads its
 *          geometry, binds it as a 4-bpp source through ::ra8_ta_priv_on_geom,
 *          carves the decoded RGBA frame and libwebp scratch after the source
 *          bytes, decodes heap-free through the `ra8_webp` facade, and bands
 *          the frame out through ::ra8_ta_priv_on_rows. Every carve is
 *          bounds-checked; any shortfall fails closed. WebP is not
 *          stripe-decodable (its lossless mode back-references the whole
 *          frame), so it carries an honest whole-frame memory cost isolated in
 *          `webp_work`; the emitted atlas bytes are identical to the streaming
 *          arms for the same pixels.
 *
 * @param[in,out] st  Producer state (`priv_init_state()` succeeded).
 * @param[in,out] pfx Prefix-replay pull adapter over the source.
 *
 * @return Result code.
 * @retval k_ra8_ok                    Whole WebP decoded and accumulated.
 * @retval k_ra8_err_not_supported     No `webp_work` arena, or an axis over cap.
 * @retval k_ra8_err_invalid_size      The arena cannot fit source + frame + scratch.
 * @retval k_ra8_err_validation_failed Corrupt WebP or the scratch exhausted.
 * @retval other                       Propagated from the pull / facade / rows.
 *
 * @pre `ra8_ta_priv_prefix_pull` replays the sniffed head before the source.
 * @pre The dispatcher confirmed the RIFF+WEBP head.
 * @post On success every source row reached the band accumulator.
 * @post On error the transcode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t ra8_ta_priv_webp_transcode(ra8_ta_prod_state_t* st, ra8_ta_prefix_pull_t* pfx);
