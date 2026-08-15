/**
 * @file ra8_jof_png_internal.h
 * @brief Module-private declarations shared by the two PNG decoder units.
 * @ingroup grp_ereader
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * The streaming PNG decoder is split to stay under the maintainability line
 * cap:
 *
 *   - `ra8_jof_png_chunk.c` -- byte source helpers plus the chunk
 *     layer: signature/IHDR prologue, PLTE/tRNS parsing, ancillary skipping
 *     and the post-IDAT walk to IEND.
 *   - `ra8_jof_png.c`       -- the pixel layer: geometry binding, the
 *     tinfl inflate loop, scanline unfiltering/translation and the public
 *     `priv_jof_png_rows()` entry.
 *
 * This header carries the shared decode-state type, the structural
 * constants, and the chunk-layer prototypes the pixel layer drives.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "miniz.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_jof_internal.h"

/**
 * @enum ra8_png_const_t
 * @brief PNG structural sizes and caps (PNG sec 5 "Datastream structure").
 */
typedef enum : uint32_t {
  k_ra8_png_sig_bytes   = 8U,          /**< Signature length.                */
  k_ra8_png_chunk_hdr   = 8U,          /**< Chunk length + type fields.      */
  k_ra8_png_crc_bytes   = 4U,          /**< Trailing chunk CRC field.        */
  k_ra8_png_ihdr_len    = 13U,         /**< IHDR payload length.             */
  k_ra8_png_max_chunks  = 4096U,       /**< Chunk-walk budget (hostile cap). */
  k_ra8_png_max_len     = 0x7FFFFFFFU, /**< Max legal chunk length.          */
  k_ra8_png_ring_bytes  = 65536U,      /**< Inflate ring (power of 2).       */
  k_ra8_png_inbuf_bytes = 4096U,       /**< Compressed-input chunk buffer.   */
  k_ra8_png_plte_max    = 768U,        /**< Max PLTE payload (256 * 3).      */
  k_ra8_png_trns_max    = 256U,        /**< Max palette tRNS payload.        */
  k_ra8_png_skip_chunk  = 256U,        /**< Discard-buffer granularity.      */
  k_ra8_png_type_ihdr   = 0x49484452U, /**< "IHDR" big-endian.               */
  k_ra8_png_type_plte   = 0x504C5445U, /**< "PLTE" big-endian.               */
  k_ra8_png_type_idat   = 0x49444154U, /**< "IDAT" big-endian.               */
  k_ra8_png_type_iend   = 0x49454E44U, /**< "IEND" big-endian.               */
  k_ra8_png_type_trns   = 0x74524E53U, /**< "tRNS" big-endian.               */
} ra8_png_const_t;

/**
 * @enum ra8_png_field_t
 * @brief IHDR field offsets and legal values (PNG sec 11.2.2 "IHDR").
 */
typedef enum : uint8_t {
  k_ra8_png_ihdr_ofs_depth     = 8U,   /**< Bit-depth byte offset.      */
  k_ra8_png_ihdr_ofs_color     = 9U,   /**< Colour-type byte offset.    */
  k_ra8_png_ihdr_ofs_compress  = 10U,  /**< Compression-method offset.  */
  k_ra8_png_ihdr_ofs_filter    = 11U,  /**< Filter-method offset.       */
  k_ra8_png_ihdr_ofs_interlace = 12U,  /**< Interlace-method offset.    */
  k_ra8_png_depth_8            = 8U,   /**< Only supported bit depth.   */
  k_ra8_png_color_gray         = 0U,   /**< Grayscale.                  */
  k_ra8_png_color_rgb          = 2U,   /**< Truecolour.                 */
  k_ra8_png_color_pal          = 3U,   /**< Indexed-colour.             */
  k_ra8_png_color_ga           = 4U,   /**< Grayscale + alpha.          */
  k_ra8_png_color_rgba         = 6U,   /**< Truecolour + alpha.         */
  k_ra8_png_filter_none        = 0U,   /**< Row filter: none.           */
  k_ra8_png_filter_sub         = 1U,   /**< Row filter: sub.            */
  k_ra8_png_filter_up          = 2U,   /**< Row filter: up.             */
  k_ra8_png_filter_avg         = 3U,   /**< Row filter: average.        */
  k_ra8_png_filter_paeth       = 4U,   /**< Row filter: Paeth.          */
  k_ra8_png_ch_1               = 1U,   /**< 1 byte per pixel.           */
  k_ra8_png_ch_2               = 2U,   /**< 2 bytes per pixel.          */
  k_ra8_png_ch_3               = 3U,   /**< 3 bytes per pixel.          */
  k_ra8_png_ch_4               = 4U,   /**< 4 bytes per pixel.          */
  k_ra8_png_be_sh24            = 24U,  /**< Big-endian assembly shifts. */
  k_ra8_png_be_sh16            = 16U,  /**< Big-endian assembly shifts. */
  k_ra8_png_be_sh8             = 8U,   /**< Big-endian assembly shifts. */
  k_ra8_png_opaque             = 255U, /**< Fully-opaque alpha value.   */
} ra8_png_field_t;

/**
 * @struct ra8_png_state_t
 * @brief Whole streaming PNG decode state (module-static instance).
 *
 * @details One static instance: the inflate ring pointer set plus palette
 *          tables would blow the stack budget. All heap-sized buffers are
 *          carved from the producer's bump arena.
 *
 * @invariant `rowfill <= rowlen` and `rows_done <= h` at all times.
 */
typedef struct {
  ra8_jof_pull_fn pull;     /**< Sequential byte source. */
  void*           pull_ctx; /**< Context for `pull`.     */
  ra8_jof_geom_fn on_geom;  /**< Producer geometry hook. */
  ra8_jof_rows_fn on_rows;  /**< Producer row sink.      */
  void*           cb_ctx;   /**< Producer context.       */

  uint16_t w;          /**< Image width, pixels.                 */
  uint16_t h;          /**< Image height, pixels.                */
  uint8_t  color_type; /**< IHDR colour type.                    */
  uint8_t  src_ch;     /**< Source bytes per pixel (filter bpp). */
  uint8_t  dst_ch;     /**< Output bytes per pixel (1, 3 or 4).  */

  uint8_t  palette[k_ra8_png_plte_max]; /**< PLTE payload (RGB triples). */
  uint8_t  trns[k_ra8_png_trns_max];    /**< Palette alpha (tRNS).       */
  uint16_t plte_count;                  /**< Palette entries present.    */
  uint16_t trns_count;                  /**< tRNS entries present.       */
  uint8_t  has_plte;                    /**< 1 once PLTE parsed.         */
  uint8_t  has_trns;                    /**< 1 once tRNS parsed.         */

  tinfl_decompressor* inflator; /**< Carved inflate context.            */
  uint8_t*            ring;     /**< Carved 64 KiB inflate ring.        */
  uint8_t*            inbuf;    /**< Carved compressed-input buffer.    */
  uint8_t*            rowbuf;   /**< Carved scanline (filter + pixels). */
  uint8_t*            prevrow;  /**< Carved previous unfiltered row.    */
  uint8_t*            xlat;     /**< Carved translated output row.      */

  uint32_t ring_wr;   /**< Ring write offset.               */
  uint32_t rowlen;    /**< Scanline bytes (1 + w * src_ch). */
  uint32_t rowfill;   /**< Scanline bytes assembled so far. */
  uint16_t rows_done; /**< Rows emitted so far.             */

  uint32_t idat_rem;      /**< Bytes left in the current IDAT chunk.   */
  uint8_t  source_done;   /**< 1 once a non-IDAT chunk ended the data. */
  uint8_t  pending_valid; /**< 1 when a pending chunk header is held.  */
  uint32_t pending_len;   /**< Pending chunk payload length.           */
  uint32_t pending_type;  /**< Pending chunk type (big-endian value).  */
} ra8_png_state_t;

/* ---------------------------------------------------------------------------
 * Chunk-layer primitives (defined in ra8_jof_png_chunk.c, driven by
 * the pixel layer in ra8_jof_png.c).
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Pull exactly @p len bytes; EOF mid-read is a protocol error.
 * @details Bounded by @p len (each non-EOF pull delivers >= 1 byte).
 * @param[in,out] st  Decoder state (source position advances).
 * @param[out]    buf Destination buffer.
 * @param[in]     len Bytes required.
 * @return Result code.
 * @retval k_ra8_ok                 Exactly @p len bytes delivered.
 * @retval k_ra8_err_protocol_error The source ended early.
 * @retval other                    Propagated from the pull callback.
 * @pre @p buf holds @p len writable bytes.
 * @pre `st->pull` is bound.
 * @post On success the source advanced by @p len bytes.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_jof_png_pull_exact(ra8_png_state_t* st, uint8_t* buf, uint32_t len);

/**
 * @brief Discard exactly @p len source bytes (unknown / ancillary chunks).
 * @details Bounded by @p len over a fixed-size stack scratch.
 * @param[in,out] st  Decoder state (source position advances).
 * @param[in]     len Bytes to discard.
 * @return Result code.
 * @retval k_ra8_ok                 Bytes discarded.
 * @retval k_ra8_err_protocol_error The source ended early.
 * @retval other                    Propagated from the pull callback.
 * @pre `st->pull` is bound.
 * @pre @p len came from a validated chunk-length field.
 * @post On success the source advanced by @p len bytes.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_jof_png_skip(ra8_png_state_t* st, uint32_t len);

/**
 * @brief Read one 8-byte chunk header (length + type, both big-endian).
 * @details Assembles the fields and enforces the spec length cap.
 * @param[in,out] st       Decoder state (source position advances).
 * @param[out]    out_len  Receives the payload length.
 * @param[out]    out_type Receives the chunk type value.
 * @return Result code.
 * @retval k_ra8_ok                 Header read; length within the spec cap.
 * @retval k_ra8_err_protocol_error Truncated header or oversize length.
 * @retval other                    Propagated from the pull callback.
 * @pre The source is positioned at a chunk boundary.
 * @pre Both outputs are writable.
 * @post On success the source sits at the chunk payload.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_jof_png_chunk_hdr(ra8_png_state_t* st,
                                          uint32_t*        out_len,
                                          uint32_t*        out_type);

/**
 * @brief Verify the 8-byte signature, then parse + validate the IHDR.
 * @details Fail-closed prologue for the untrusted stream.
 * @param[in,out] st    Decoder state (geometry fields written).
 * @param[in]     max_w Fail-closed width cap.
 * @param[in]     max_h Fail-closed height cap.
 * @return Result code.
 * @retval k_ra8_ok                 Prologue accepted; chunk walk may start.
 * @retval k_ra8_err_protocol_error Bad signature / IHDR structure.
 * @retval other                    Propagated from the IHDR validation.
 * @pre The source is positioned at byte 0 of the PNG stream.
 * @pre @p st holds the bound callbacks.
 * @post On success the source sits at the first post-IHDR chunk.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_jof_png_prologue(ra8_png_state_t* st, uint16_t max_w, uint16_t max_h);

/**
 * @brief Dispatch one pre-IDAT chunk (PLTE / tRNS / ancillary / stray IEND).
 * @details PLTE and tRNS bind the palette tables; unknown chunks skip.
 * @param[in,out] st   Decoder state.
 * @param[in]     len  Chunk payload length.
 * @param[in]     type Chunk type value.
 * @return Result code.
 * @retval k_ra8_ok                 Chunk consumed; keep walking.
 * @retval k_ra8_err_protocol_error IEND arrived before any IDAT.
 * @retval other                    Propagated from the chunk parsers.
 * @pre The IHDR has been parsed.
 * @pre @p len came from a validated chunk header.
 * @post On success the source sits at the next chunk boundary.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_jof_png_pre_idat(ra8_png_state_t* st, uint32_t len, uint32_t type);

/**
 * @brief Walk the post-IDAT chunks until IEND (ancillary chunks skipped).
 * @details When the zlib stream ended inside the last IDAT (no pending
 *          chunk parked by the refill path), that IDAT's trailing CRC is
 *          still unread: consume it, then resume the chunk walk.
 * @param[in,out] st Decoder state (pending chunk consumed first).
 * @return Result code.
 * @retval k_ra8_ok                 IEND reached.
 * @retval k_ra8_err_protocol_error Chunk budget exhausted / truncation /
 *                                  a stray IDAT after the stream ended.
 * @retval other                    Propagated from the pull callback.
 * @pre The inflate phase finished (`rows_done == h`).
 * @pre `st->idat_rem == 0`.
 * @post On success the datastream is fully consumed through IEND.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV ra8_err_t priv_jof_png_finish(ra8_png_state_t* st);
