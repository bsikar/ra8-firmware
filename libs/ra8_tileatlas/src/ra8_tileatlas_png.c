/**
 * @file ra8_tileatlas_png.c
 * @brief Streaming PNG scanline decoder for the transcode producer (#231).
 *
 * @details
 * Implements `ra8_ta_priv_png_rows()`: a bounded-RAM, pull-based PNG decoder
 * that never holds the whole image. IDAT deflate data inflates through
 * miniz `tinfl` into a 64 KiB power-of-two ring (the LZ dictionary), bytes
 * drain into a single scanline assembly buffer, each completed scanline is
 * unfiltered against one previous row (PNG spec sec 9 "Filtering"), then
 * translated to the producer's output layout and emitted -- resident cost is
 * the ring plus three rows, independent of the image height.
 *
 * Supported: 8-bit depth, colour types 0 (gray), 2 (RGB), 3 (palette,
 * with/without tRNS), 4 (gray+alpha), 6 (RGBA), non-interlaced. Everything
 * else is rejected fail-closed -- this decoder feeds on untrusted EPUB
 * content. Spec citations reference the W3C PNG specification (second
 * edition), abbreviated `PNG sec N`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 4 / Domain]
 * {World: NS}
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "miniz.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_tileatlas_internal.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_ta_png";

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

/** @brief PNG signature (PNG sec 5.2). */
static const uint8_t s_png_sig[k_ra8_png_sig_bytes] =
  {0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU}; /* MAGIC-OK: the 8 spec signature bytes */

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
  ra8_tileatlas_pull_fn pull;     /**< Sequential byte source. */
  void*                 pull_ctx; /**< Context for `pull`.     */
  ra8_ta_geom_fn        on_geom;  /**< Producer geometry hook. */
  ra8_ta_rows_fn        on_rows;  /**< Producer row sink.      */
  void*                 cb_ctx;   /**< Producer context.       */

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

/** @brief Module-static decode state (decoder documented not thread-safe). */
static ra8_png_state_t s_png;

/* ---------------------------------------------------------------------------
 * Byte-source helpers.
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
 * @pre @p st->pull is bound.
 * @post On success the source advanced by @p len bytes.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t png_pull_exact(ra8_png_state_t* st, uint8_t* buf, uint32_t len)
{
  uint32_t done = 0U;
  while (done < len) {
    size_t          got = 0U;
    const ra8_err_t err = st->pull(st->pull_ctx, &buf[done], (size_t)(len - done), &got);
    if (err != k_ra8_ok) {
      return err;
    }
    if (got == 0U) {
      return k_ra8_err_protocol_error;
    }
    done += (uint32_t)got;
  }
  return k_ra8_ok;
}

/**
 * @brief Discard exactly @p len source bytes (unknown / ancillary chunks).
 * @details Bounded by @p len over a fixed-size stack scratch.
 * @param[in,out] st  Decoder state (source position advances).
 * @param[in]     len Bytes to discard.
 * @return Result code.
 * @retval k_ra8_ok                 Bytes discarded.
 * @retval k_ra8_err_protocol_error The source ended early.
 * @retval other                    Propagated from the pull callback.
 * @pre @p st->pull is bound.
 * @pre @p len came from a validated chunk-length field.
 * @post On success the source advanced by @p len bytes.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t png_skip(ra8_png_state_t* st, uint32_t len)
{
  uint8_t  scratch[k_ra8_png_skip_chunk];
  uint32_t left = len;
  while (left > 0U) {
    const uint32_t take =
      (left < (uint32_t)k_ra8_png_skip_chunk) ? left : (uint32_t)k_ra8_png_skip_chunk;
    const ra8_err_t err = png_pull_exact(st, scratch, take);
    if (err != k_ra8_ok) {
      return err;
    }
    left -= take;
  }
  return k_ra8_ok;
}

/**
 * @brief Read one 8-byte chunk header (length + type, both big-endian).
 * @details Assembles the big-endian length and type fields and enforces the spec length cap.
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
static ra8_err_t png_read_chunk_hdr(ra8_png_state_t* st, uint32_t* out_len, uint32_t* out_type)
{
  uint8_t         hdr[k_ra8_png_chunk_hdr] = {};
  const ra8_err_t err                      = png_pull_exact(st, hdr, sizeof(hdr));
  if (err != k_ra8_ok) {
    return err;
  }
  const uint32_t len  = ((uint32_t)hdr[0] << k_ra8_png_be_sh24) |
                        ((uint32_t)hdr[1] << k_ra8_png_be_sh16) |
                        ((uint32_t)hdr[2] << k_ra8_png_be_sh8) | (uint32_t)hdr[3];
  const uint32_t type = ((uint32_t)hdr[4] << k_ra8_png_be_sh24) |
                        ((uint32_t)hdr[5] << k_ra8_png_be_sh16) |
                        ((uint32_t)hdr[6] << k_ra8_png_be_sh8) | (uint32_t)hdr[7];
  if (len > (uint32_t)k_ra8_png_max_len) {
    return k_ra8_err_protocol_error;
  }
  *out_len  = len;
  *out_type = type;
  return k_ra8_ok;
}

/* ---------------------------------------------------------------------------
 * IHDR / PLTE / tRNS parsing.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Map an IHDR colour type to its source bytes-per-pixel, or 0.
 * @details The bit depth is fixed at 8, so bytes per pixel equals the channel count.
 * @param[in] color_type IHDR colour-type byte.
 * @return Source channel count for 8-bit depth.
 * @retval 1-4 Legal colour type's bytes per pixel.
 * @retval 0   Unsupported / illegal colour type.
 * @pre The bit depth was validated as 8.
 * @pre None (total over uint8_t).
 * @post No state mutated.
 * @post Return is 0 or a legal bpp.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static uint8_t png_src_channels(uint8_t color_type)
{
  switch (color_type) {
    case k_ra8_png_color_gray:
      return (uint8_t)k_ra8_png_ch_1;
    case k_ra8_png_color_rgb:
      return (uint8_t)k_ra8_png_ch_3;
    case k_ra8_png_color_pal:
      return (uint8_t)k_ra8_png_ch_1;
    case k_ra8_png_color_ga:
      return (uint8_t)k_ra8_png_ch_2;
    case k_ra8_png_color_rgba:
      return (uint8_t)k_ra8_png_ch_4;
    default:
      return 0U;
  }
}

/**
 * @brief Parse + validate the IHDR chunk into the decode state.
 * @details Rejects, fail-closed: non-8-bit depth, unknown colour types,
 *          non-zero compression/filter methods, interlacing, zero or
 *          over-cap dimensions (PNG sec 11.2.2).
 * @param[in,out] st    Decoder state (geometry fields written).
 * @param[in]     max_w Fail-closed width cap.
 * @param[in]     max_h Fail-closed height cap.
 * @return Result code.
 * @retval k_ra8_ok                 IHDR accepted.
 * @retval k_ra8_err_invalid_size   Dimension zero / over the caps.
 * @retval k_ra8_err_not_supported  Depth / colour / method / interlace.
 * @retval k_ra8_err_protocol_error Structural mismatch (length, order).
 * @retval other                    Propagated from the pull callback.
 * @pre The source sits right after the PNG signature.
 * @pre @p st is zero-initialised apart from the bound callbacks.
 * @post On success `w`/`h`/`color_type`/`src_ch` are set.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t png_parse_ihdr(ra8_png_state_t* st, uint16_t max_w, uint16_t max_h)
{
  uint32_t  len  = 0U;
  uint32_t  type = 0U;
  ra8_err_t err  = png_read_chunk_hdr(st, &len, &type);
  if (err != k_ra8_ok) {
    return err;
  }
  if ((type != (uint32_t)k_ra8_png_type_ihdr) || (len != (uint32_t)k_ra8_png_ihdr_len)) {
    return k_ra8_err_protocol_error;
  }
  uint8_t ihdr[k_ra8_png_ihdr_len] = {};
  err                              = png_pull_exact(st, ihdr, sizeof(ihdr));
  if (err != k_ra8_ok) {
    return err;
  }
  const uint32_t w32 = ((uint32_t)ihdr[0] << k_ra8_png_be_sh24) |
                       ((uint32_t)ihdr[1] << k_ra8_png_be_sh16) |
                       ((uint32_t)ihdr[2] << k_ra8_png_be_sh8) | (uint32_t)ihdr[3];
  const uint32_t h32 = ((uint32_t)ihdr[4] << k_ra8_png_be_sh24) |
                       ((uint32_t)ihdr[5] << k_ra8_png_be_sh16) |
                       ((uint32_t)ihdr[6] << k_ra8_png_be_sh8) | (uint32_t)ihdr[7];
  if ((w32 == 0U) || (w32 > (uint32_t)max_w)) {
    return k_ra8_err_invalid_size;
  }
  if ((h32 == 0U) || (h32 > (uint32_t)max_h)) {
    return k_ra8_err_invalid_size;
  }
  if (ihdr[k_ra8_png_ihdr_ofs_depth] != (uint8_t)k_ra8_png_depth_8) {
    return k_ra8_err_not_supported;
  }
  st->src_ch = png_src_channels(ihdr[k_ra8_png_ihdr_ofs_color]);
  if (st->src_ch == 0U) {
    return k_ra8_err_not_supported;
  }
  if (ihdr[k_ra8_png_ihdr_ofs_compress] != 0U) {
    return k_ra8_err_not_supported;
  }
  if (ihdr[k_ra8_png_ihdr_ofs_filter] != 0U) {
    return k_ra8_err_not_supported;
  }
  if (ihdr[k_ra8_png_ihdr_ofs_interlace] != 0U) {
    return k_ra8_err_not_supported; /* Adam7 breaks streaming; fail closed */
  }
  st->w          = (uint16_t)w32;
  st->h          = (uint16_t)h32;
  st->color_type = ihdr[k_ra8_png_ihdr_ofs_color];
  return png_skip(st, (uint32_t)k_ra8_png_crc_bytes);
}

/**
 * @brief Parse a PLTE chunk into the palette table (colour type 3).
 * @details Validates the payload shape, stores the RGB triples, and skips the trailing CRC.
 * @param[in,out] st  Decoder state (palette written).
 * @param[in]     len PLTE payload length.
 * @return Result code.
 * @retval k_ra8_ok                    Palette stored.
 * @retval k_ra8_err_validation_failed Length not a multiple of 3 / over cap
 *                                     / duplicate PLTE.
 * @retval other                       Propagated from the pull callback.
 * @pre IHDR has been parsed.
 * @pre @p len came from a validated chunk header.
 * @post On success `plte_count` reflects the stored entries.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t png_parse_plte(ra8_png_state_t* st, uint32_t len)
{
  if ((len == 0U) || ((len % (uint32_t)k_ra8_png_ch_3) != 0U) ||
      (len > (uint32_t)k_ra8_png_plte_max) || (st->has_plte != 0U)) {
    return k_ra8_err_validation_failed;
  }
  const ra8_err_t err = png_pull_exact(st, st->palette, len);
  if (err != k_ra8_ok) {
    return err;
  }
  st->plte_count = (uint16_t)(len / (uint32_t)k_ra8_png_ch_3);
  st->has_plte   = 1U;
  return png_skip(st, (uint32_t)k_ra8_png_crc_bytes);
}

/**
 * @brief Parse a tRNS chunk (palette alpha; colour type 3 only).
 * @details A tRNS on colour types 0/2 (colour-key transparency) is rejected
 *          `k_ra8_err_not_supported` rather than silently flattened -- this
 *          producer never alters pixels it cannot represent.
 * @param[in,out] st  Decoder state (alpha table written).
 * @param[in]     len tRNS payload length.
 * @return Result code.
 * @retval k_ra8_ok                    Alpha table stored.
 * @retval k_ra8_err_not_supported     tRNS on a non-palette colour type.
 * @retval k_ra8_err_validation_failed Oversize / missing PLTE / duplicate.
 * @retval other                       Propagated from the pull callback.
 * @pre IHDR has been parsed.
 * @pre @p len came from a validated chunk header.
 * @post On success `trns_count`/`has_trns` reflect the table.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t png_parse_trns(ra8_png_state_t* st, uint32_t len)
{
  if (st->color_type != (uint8_t)k_ra8_png_color_pal) {
    return k_ra8_err_not_supported;
  }
  if ((len > (uint32_t)k_ra8_png_trns_max) || (st->has_plte == 0U) || (st->has_trns != 0U)) {
    return k_ra8_err_validation_failed;
  }
  const ra8_err_t err = png_pull_exact(st, st->trns, len);
  if (err != k_ra8_ok) {
    return err;
  }
  st->trns_count = (uint16_t)len;
  st->has_trns   = 1U;
  return png_skip(st, (uint32_t)k_ra8_png_crc_bytes);
}

/* ---------------------------------------------------------------------------
 * Scanline reconstruction.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Paeth predictor (PNG sec 9.4 "Filter type 4: Paeth").
 * @details Selects whichever neighbour is closest to the linear predictor `a + b - c`.
 * @param[in] a Left reconstructed byte.
 * @param[in] b Above reconstructed byte.
 * @param[in] c Above-left reconstructed byte.
 * @return The predictor byte.
 * @retval a-c Whichever neighbour is closest to `a + b - c`.
 * @pre None (total over uint8_t inputs).
 * @pre None.
 * @post No state mutated.
 * @post Return is one of the three inputs.
 * @note Pure; thread-safe.
 * @since 0.1.0
 */
static uint8_t png_paeth(uint8_t a, uint8_t b, uint8_t c)
{
  const int32_t p  = (int32_t)a + (int32_t)b - (int32_t)c;
  const int32_t pa = (p > (int32_t)a) ? (p - (int32_t)a) : ((int32_t)a - p);
  const int32_t pb = (p > (int32_t)b) ? (p - (int32_t)b) : ((int32_t)b - p);
  const int32_t pc = (p > (int32_t)c) ? (p - (int32_t)c) : ((int32_t)c - p);
  if ((pa <= pb) && (pa <= pc)) {
    return a;
  }
  if (pb <= pc) {
    return b;
  }
  return c;
}

/**
 * @brief Reconstruct one filtered scanline in place (PNG sec 9 filters).
 * @details `row` is the raw scanline (post filter-byte); `prev` is the
 *          previous reconstructed row (all zeroes for the first row, per
 *          spec). The filter arithmetic is modulo-256 by construction.
 * @param[in]     filter Filter-type byte (0..4; caller-validated range in).
 * @param[in,out] row    Scanline bytes, reconstructed in place.
 * @param[in]     prev   Previous reconstructed row.
 * @param[in]     nbytes Scanline byte count (`w * src_ch`).
 * @param[in]     bpp    Filter delta distance (source bytes per pixel).
 * @return Result code.
 * @retval k_ra8_ok                    Row reconstructed.
 * @retval k_ra8_err_validation_failed Unknown filter type (hostile stream).
 * @pre @p row and @p prev each cover @p nbytes bytes.
 * @pre @p bpp is in 1..4.
 * @post On success @p row holds reconstructed bytes.
 * @post On error the row content is unspecified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t
png_unfilter(uint8_t filter, uint8_t* row, const uint8_t* prev, uint32_t nbytes, uint8_t bpp)
{
  switch (filter) {
    case k_ra8_png_filter_none:
      return k_ra8_ok;
    case k_ra8_png_filter_sub:
      for (uint32_t i = (uint32_t)bpp; i < nbytes; i++) {
        row[i] = (uint8_t)(row[i] + row[i - bpp]);
      }
      return k_ra8_ok;
    case k_ra8_png_filter_up:
      for (uint32_t i = 0U; i < nbytes; i++) {
        row[i] = (uint8_t)(row[i] + prev[i]);
      }
      return k_ra8_ok;
    case k_ra8_png_filter_avg:
      for (uint32_t i = 0U; i < nbytes; i++) {
        const uint32_t left = (i >= (uint32_t)bpp) ? (uint32_t)row[i - bpp] : 0U;
        row[i]              = (uint8_t)(row[i] + (uint8_t)((left + (uint32_t)prev[i]) / 2U));
      }
      return k_ra8_ok;
    case k_ra8_png_filter_paeth:
      for (uint32_t i = 0U; i < nbytes; i++) {
        const uint8_t a = (i >= (uint32_t)bpp) ? row[i - bpp] : 0U;
        const uint8_t c = (i >= (uint32_t)bpp) ? prev[i - bpp] : 0U;
        row[i]          = (uint8_t)(row[i] + png_paeth(a, prev[i], c));
      }
      return k_ra8_ok;
    default:
      return k_ra8_err_validation_failed;
  }
}

/**
 * @brief Translate one reconstructed source row into the output layout.
 * @details gray -> gray8; RGB/RGBA -> copied; gray+alpha -> RGBA (the gray
 *          value replicated); palette -> RGB or RGBA via PLTE (+ tRNS),
 *          with every index bounds-checked against the palette (hostile
 *          streams may index past a short PLTE).
 * @param[in,out] st  Decoder state (`xlat` written).
 * @param[in]     row Reconstructed source row (`w * src_ch` bytes).
 * @return Result code.
 * @retval k_ra8_ok                    Row translated into `st->xlat`.
 * @retval k_ra8_err_validation_failed A palette index is out of range.
 * @pre `st->xlat` covers `w * dst_ch` bytes.
 * @pre `st->dst_ch` was fixed at geometry time.
 * @post On success `st->xlat` holds the packed output row.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t png_translate(ra8_png_state_t* st, const uint8_t* row)
{
  const uint32_t w = (uint32_t)st->w;
  if (st->color_type == (uint8_t)k_ra8_png_color_pal) {
    for (uint32_t x = 0U; x < w; x++) {
      const uint8_t idx = row[x];
      if ((uint16_t)idx >= st->plte_count) {
        return k_ra8_err_validation_failed;
      }
      const uint32_t src = (uint32_t)idx * (uint32_t)k_ra8_png_ch_3;
      const uint32_t dst = x * (uint32_t)st->dst_ch;
      st->xlat[dst]      = st->palette[src];
      st->xlat[dst + 1U] = st->palette[src + 1U];
      st->xlat[dst + 2U] = st->palette[src + 2U];
      if (st->dst_ch == (uint8_t)k_ra8_png_ch_4) {
        st->xlat[dst + 3U] =
          ((uint16_t)idx < st->trns_count) ? st->trns[idx] : (uint8_t)k_ra8_png_opaque;
      }
    }
    return k_ra8_ok;
  }
  if (st->color_type == (uint8_t)k_ra8_png_color_ga) {
    for (uint32_t x = 0U; x < w; x++) {
      const uint8_t  g   = row[x * (uint32_t)k_ra8_png_ch_2];
      const uint8_t  a   = row[(x * (uint32_t)k_ra8_png_ch_2) + 1U];
      const uint32_t dst = x * (uint32_t)k_ra8_png_ch_4;
      st->xlat[dst]      = g;
      st->xlat[dst + 1U] = g;
      st->xlat[dst + 2U] = g;
      st->xlat[dst + 3U] = a;
    }
    return k_ra8_ok;
  }
  /* gray, RGB, RGBA: source layout == output layout. */
  (void)memcpy(st->xlat, row, (size_t)w * (size_t)st->dst_ch);
  return k_ra8_ok;
}

/**
 * @brief Assemble, reconstruct and emit rows from freshly inflated bytes.
 * @details Drains `avail` produced bytes into the scanline buffer; each
 *          completed scanline is unfiltered, saved as the new previous row,
 *          translated and emitted. Producing more scanlines than IHDR
 *          declared is a hostile-stream error.
 * @param[in,out] st    Decoder state.
 * @param[in]     src   Freshly produced ring bytes (contiguous).
 * @param[in]     avail Byte count at @p src.
 * @return Result code.
 * @retval k_ra8_ok                    Bytes consumed (rows possibly emitted).
 * @retval k_ra8_err_validation_failed Extra rows / bad filter / bad index.
 * @retval other                       Propagated from the row sink.
 * @pre Geometry has been bound (buffers carved).
 * @pre @p src covers @p avail bytes.
 * @post `rowfill`/`rows_done` advanced per the consumed bytes.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t png_consume_rows(ra8_png_state_t* st, const uint8_t* src, uint32_t avail)
{
  uint32_t pos = 0U;
  while (pos < avail) {
    const uint32_t need = st->rowlen - st->rowfill;
    const uint32_t left = avail - pos;
    const uint32_t take = (left < need) ? left : need;
    (void)memcpy(&st->rowbuf[st->rowfill], &src[pos], (size_t)take);
    st->rowfill += take;
    pos += take;
    if (st->rowfill < st->rowlen) {
      break; /* partial scanline; wait for more inflate output */
    }
    if (st->rows_done >= st->h) {
      return k_ra8_err_validation_failed; /* more scanlines than IHDR said */
    }
    const uint32_t nbytes = st->rowlen - 1U;
    ra8_err_t err = png_unfilter(st->rowbuf[0], &st->rowbuf[1], st->prevrow, nbytes, st->src_ch);
    if (err != k_ra8_ok) {
      return err;
    }
    (void)memcpy(st->prevrow, &st->rowbuf[1], (size_t)nbytes);
    err = png_translate(st, &st->rowbuf[1]);
    if (err != k_ra8_ok) {
      return err;
    }
    err = st->on_rows(st->cb_ctx, st->xlat, st->w, st->rows_done, 1U, st->dst_ch);
    if (err != k_ra8_ok) {
      return err;
    }
    st->rows_done++;
    st->rowfill = 0U;
  }
  return k_ra8_ok;
}

/* ---------------------------------------------------------------------------
 * IDAT inflate phase.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief Fire the geometry hook and carve the pixel-path buffers.
 * @details Runs once, at the first IDAT: decides the output channel count
 *          (palette + tRNS promotes to RGBA), requires PLTE for palette
 *          images, and carves the inflate + row buffers from the arena.
 * @param[in,out] st   Decoder state.
 * @param[in,out] bump Work-arena allocator.
 * @return Result code.
 * @retval k_ra8_ok                    Buffers carved; hook accepted.
 * @retval k_ra8_err_validation_failed Palette image with no PLTE.
 * @retval k_ra8_err_invalid_size      Arena exhausted (budget fail-closed).
 * @retval other                       The geometry hook aborted the decode.
 * @pre IHDR (and any PLTE/tRNS) have been parsed.
 * @pre @p bump has the PNG carve set available.
 * @post On success all pixel-path buffers are bound.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t png_bind_geometry(ra8_png_state_t* st, ra8_ta_bump_t* bump)
{
  if (st->color_type == (uint8_t)k_ra8_png_color_pal) {
    if (st->has_plte == 0U) {
      return k_ra8_err_validation_failed;
    }
    st->dst_ch = (st->has_trns != 0U) ? (uint8_t)k_ra8_png_ch_4 : (uint8_t)k_ra8_png_ch_3;
  } else if (st->color_type == (uint8_t)k_ra8_png_color_gray) {
    st->dst_ch = (uint8_t)k_ra8_png_ch_1;
  } else if (st->color_type == (uint8_t)k_ra8_png_color_ga) {
    st->dst_ch = (uint8_t)k_ra8_png_ch_4;
  } else {
    st->dst_ch = st->src_ch; /* RGB -> 3, RGBA -> 4 */
  }
  const ra8_err_t err = st->on_geom(st->cb_ctx, st->w, st->h, st->dst_ch);
  if (err != k_ra8_ok) {
    return err;
  }
  st->rowlen = 1U + ((uint32_t)st->w * (uint32_t)st->src_ch);
  /* The bump allocator returns 8-byte-aligned carves, which satisfies the
   * inflate context's alignment; the void* hop silences -Wcast-align. */
  st->inflator =
    (tinfl_decompressor*)(void*)ra8_ta_priv_bump_take(bump, sizeof(tinfl_decompressor));
  st->ring    = ra8_ta_priv_bump_take(bump, (size_t)k_ra8_png_ring_bytes);
  st->inbuf   = ra8_ta_priv_bump_take(bump, (size_t)k_ra8_png_inbuf_bytes);
  st->rowbuf  = ra8_ta_priv_bump_take(bump, (size_t)st->rowlen);
  st->prevrow = ra8_ta_priv_bump_take(bump, (size_t)(st->rowlen - 1U));
  st->xlat    = ra8_ta_priv_bump_take(bump, (size_t)st->w * (size_t)st->dst_ch);
  if ((st->inflator == nullptr) || (st->ring == nullptr) || (st->inbuf == nullptr) ||
      (st->rowbuf == nullptr) || (st->prevrow == nullptr) || (st->xlat == nullptr)) {
    return k_ra8_err_invalid_size;
  }
  (void)memset(st->prevrow, 0, (size_t)(st->rowlen - 1U)); /* PNG sec 9: prior row = zeroes */
  tinfl_init(st->inflator);
  return k_ra8_ok;
}

/**
 * @brief Refill the compressed-input buffer from the IDAT chunk stream.
 * @details Crosses consecutive IDAT chunks transparently (skipping each
 *          CRC); a non-IDAT chunk ends the compressed stream and is parked
 *          as the pending chunk for the outer walk. Bounded by the chunk
 *          budget shared with the outer walk.
 * @param[in,out] st        Decoder state.
 * @param[out]    out_avail Receives the refilled byte count (0 = stream end).
 * @return Result code.
 * @retval k_ra8_ok                 Buffer refilled (or clean stream end).
 * @retval k_ra8_err_protocol_error Truncated chunk structure.
 * @retval other                    Propagated from the pull callback.
 * @pre The inflate phase is active (first IDAT seen).
 * @pre @p out_avail is writable.
 * @post `*out_avail` bytes at `st->inbuf` are compressed data.
 * @post `source_done`/`pending_*` reflect a stream-ending chunk.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t png_refill_input(ra8_png_state_t* st, uint32_t* out_avail)
{
  *out_avail = 0U;
  for (uint32_t guard = 0U; guard < (uint32_t)k_ra8_png_max_chunks; guard++) {
    if (st->idat_rem > 0U) {
      const uint32_t  take = (st->idat_rem < (uint32_t)k_ra8_png_inbuf_bytes)
                               ? st->idat_rem
                               : (uint32_t)k_ra8_png_inbuf_bytes;
      const ra8_err_t err  = png_pull_exact(st, st->inbuf, take);
      if (err != k_ra8_ok) {
        return err;
      }
      st->idat_rem -= take;
      *out_avail = take;
      return k_ra8_ok;
    }
    /* Current IDAT exhausted: skip its CRC, look at the next chunk. */
    ra8_err_t err = png_skip(st, (uint32_t)k_ra8_png_crc_bytes);
    if (err != k_ra8_ok) {
      return err;
    }
    uint32_t len  = 0U;
    uint32_t type = 0U;
    err           = png_read_chunk_hdr(st, &len, &type);
    if (err != k_ra8_ok) {
      return err;
    }
    if (type == (uint32_t)k_ra8_png_type_idat) {
      st->idat_rem = len;
      continue;
    }
    st->pending_len   = len;
    st->pending_type  = type;
    st->pending_valid = 1U;
    st->source_done   = 1U;
    return k_ra8_ok; /* clean end of the compressed stream */
  }
  return k_ra8_err_protocol_error; /* chunk budget exhausted (hostile) */
}

/**
 * @brief Inflate the whole IDAT stream, emitting every scanline.
 * @details The tinfl ring is the LZ dictionary; each call produces into the
 *          contiguous run `[ring_wr, ring_end)` which is drained into the
 *          scanline assembler before the write offset wraps. A call that
 *          neither consumes input nor produces output twice in a row is a
 *          hostile-stream abort (progress guard). Strictness: the deflate
 *          stream must end exactly when the last scanline completes, with
 *          no trailing compressed bytes.
 * @param[in,out] st        Decoder state.
 * @param[in]     first_len Payload length of the first IDAT chunk.
 * @return Result code.
 * @retval k_ra8_ok                    All rows emitted; stream ended clean.
 * @retval k_ra8_err_protocol_error    Corrupt / truncated deflate stream.
 * @retval k_ra8_err_validation_failed Row-count / trailing-byte mismatch.
 * @retval other                       Propagated from pull / the hooks.
 * @pre `png_bind_geometry()` succeeded.
 * @pre The source sits at the first IDAT's payload.
 * @post On success `rows_done == h` and the pending chunk is parked.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t png_inflate_idat(ra8_png_state_t* st, uint32_t first_len)
{
  st->idat_rem       = first_len;
  uint32_t  in_avail = 0U;
  uint32_t  in_pos   = 0U;
  uint8_t   stalls   = 0U;
  ra8_err_t err      = k_ra8_ok;
  while (true) {
    if ((in_pos == in_avail) && (st->source_done == 0U)) {
      err = png_refill_input(st, &in_avail);
      if (err != k_ra8_ok) {
        return err;
      }
      in_pos = 0U;
    }
    size_t             in_sz  = (size_t)(in_avail - in_pos);
    size_t             out_sz = (size_t)((uint32_t)k_ra8_png_ring_bytes - st->ring_wr);
    const mz_uint      flags  = (mz_uint)TINFL_FLAG_PARSE_ZLIB_HEADER |
                                ((st->source_done == 0U) ? (mz_uint)TINFL_FLAG_HAS_MORE_INPUT : 0U);
    const tinfl_status status = tinfl_decompress(st->inflator,
                                                 &st->inbuf[in_pos],
                                                 &in_sz,
                                                 st->ring,
                                                 &st->ring[st->ring_wr],
                                                 &out_sz,
                                                 flags);
    if (status < TINFL_STATUS_DONE) {
      return k_ra8_err_protocol_error; /* corrupt deflate / bad zlib header */
    }
    if ((in_sz == 0U) && (out_sz == 0U)) {
      stalls++;
      if ((stalls > 1U) || (st->source_done != 0U)) {
        return k_ra8_err_protocol_error; /* truncated stream / no progress */
      }
    } else {
      stalls = 0U;
    }
    if (out_sz > 0U) {
      err = png_consume_rows(st, &st->ring[st->ring_wr], (uint32_t)out_sz);
      if (err != k_ra8_ok) {
        return err;
      }
      st->ring_wr = (st->ring_wr + (uint32_t)out_sz) & ((uint32_t)k_ra8_png_ring_bytes - 1U);
    }
    in_pos += (uint32_t)in_sz;
    if (status == TINFL_STATUS_DONE) {
      break;
    }
  }
  /* Strict termination: every declared row arrived, nothing extra. */
  if ((st->rows_done != st->h) || (st->rowfill != 0U)) {
    return k_ra8_err_validation_failed;
  }
  if ((in_pos != in_avail) || (st->idat_rem != 0U)) {
    return k_ra8_err_validation_failed; /* trailing garbage inside IDAT */
  }
  return k_ra8_ok;
}

/* ---------------------------------------------------------------------------
 * Entry point.
 * ---------------------------------------------------------------------------
 */

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
static ra8_err_t png_finish_chunks(ra8_png_state_t* st)
{
  uint32_t len  = st->pending_len;
  uint32_t type = st->pending_type;
  if (st->pending_valid == 0U) {
    ra8_err_t err = png_skip(st, (uint32_t)k_ra8_png_crc_bytes); /* last IDAT's CRC */
    if (err != k_ra8_ok) {
      return err;
    }
    err = png_read_chunk_hdr(st, &len, &type);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  for (uint32_t guard = 0U; guard < (uint32_t)k_ra8_png_max_chunks; guard++) {
    if (type == (uint32_t)k_ra8_png_type_iend) {
      return k_ra8_ok;
    }
    if (type == (uint32_t)k_ra8_png_type_idat) {
      return k_ra8_err_protocol_error; /* IDAT after the stream completed */
    }
    ra8_err_t err = png_skip(st, len + (uint32_t)k_ra8_png_crc_bytes);
    if (err != k_ra8_ok) {
      return err;
    }
    err = png_read_chunk_hdr(st, &len, &type);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_err_protocol_error;
}

RA8_PRIV ra8_err_t ra8_ta_priv_png_rows(ra8_tileatlas_pull_fn pull,
                                        void*                 pull_ctx,
                                        ra8_ta_bump_t*        bump,
                                        uint16_t              max_w,
                                        uint16_t              max_h,
                                        ra8_ta_geom_fn        on_geom,
                                        ra8_ta_rows_fn        on_rows,
                                        void*                 cb_ctx)
{
  RA8_CHECK_NULL_PTR(pull, s_tag, "pull must not be nullptr");
  RA8_CHECK_NULL_PTR(bump, s_tag, "bump must not be nullptr");
  RA8_CHECK_NULL_PTR(on_geom, s_tag, "on_geom must not be nullptr");
  RA8_CHECK_NULL_PTR(on_rows, s_tag, "on_rows must not be nullptr");
  ra8_png_state_t* st = &s_png;
  (void)memset(st, 0, sizeof(*st));
  st->pull     = pull;
  st->pull_ctx = pull_ctx;
  st->on_geom  = on_geom;
  st->on_rows  = on_rows;
  st->cb_ctx   = cb_ctx;

  uint8_t   sig[k_ra8_png_sig_bytes] = {};
  ra8_err_t err                      = png_pull_exact(st, sig, sizeof(sig));
  if (err != k_ra8_ok) {
    return err;
  }
  if (memcmp(sig, s_png_sig, sizeof(sig)) != 0) {
    return k_ra8_err_protocol_error;
  }
  err = png_parse_ihdr(st, max_w, max_h);
  if (err != k_ra8_ok) {
    return err;
  }
  for (uint32_t guard = 0U; guard < (uint32_t)k_ra8_png_max_chunks; guard++) {
    uint32_t len  = 0U;
    uint32_t type = 0U;
    err           = png_read_chunk_hdr(st, &len, &type);
    if (err != k_ra8_ok) {
      return err;
    }
    if (type == (uint32_t)k_ra8_png_type_plte) {
      err = png_parse_plte(st, len);
    } else if (type == (uint32_t)k_ra8_png_type_trns) {
      err = png_parse_trns(st, len);
    } else if (type == (uint32_t)k_ra8_png_type_idat) {
      err = png_bind_geometry(st, bump);
      if (err != k_ra8_ok) {
        return err;
      }
      err = png_inflate_idat(st, len);
      if (err != k_ra8_ok) {
        return err;
      }
      return png_finish_chunks(st);
    } else if (type == (uint32_t)k_ra8_png_type_iend) {
      return k_ra8_err_protocol_error; /* IEND before any IDAT */
    } else {
      err = png_skip(st, len + (uint32_t)k_ra8_png_crc_bytes);
    }
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_err_protocol_error; /* chunk budget exhausted (hostile) */
}
