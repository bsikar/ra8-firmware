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
 *   - `ra8_tileatlas_produce.c` -- sniff/dispatch, band accumulator, tile
 *     cut + encode + sink, index/footer emission.
 *   - `ra8_tileatlas_png.c`     -- the streaming PNG scanline decoder.
 *
 * This header carries the bump allocator over the caller's work arena and
 * the geometry/rows callback contracts the PNG decoder reports through
 * (the JPEG path reports through `ra8_jpeg_sw_decode_stripes()`'s own
 * public seams instead).
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
 * @retval non-NULL 8-byte-aligned region of @p len bytes.
 *
 * @pre @p bump is non-NULL with `base` covering `cap` bytes.
 * @pre @p len is a real buffer size (> 0).
 * @post On success `bump->off` advanced past the carve.
 * @post On NULL the arena is unchanged.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV uint8_t* ra8_ta_priv_bump_take(ra8_ta_bump_t* bump, size_t len);

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
