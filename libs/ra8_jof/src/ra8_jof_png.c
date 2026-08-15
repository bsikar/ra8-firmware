/**
 * @file ra8_jof_png.c
 * @brief Streaming PNG scanline decoder for the transcode producer (#231).
 *
 * @details
 * Implements `priv_jof_png_rows()`: a bounded-RAM, pull-based PNG decoder
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
#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_jof_internal.h"
#include "ra8_jof_png_internal.h"
#include "ra8_log.h"

/** @brief Module log tag. */
static const char* const s_tag = "ra8_jof_png";

/** @brief Module-static decode state (decoder documented not thread-safe). */
static ra8_png_state_t s_png;

/* ---------------------------------------------------------------------------
 * Byte-source helpers.
 * ---------------------------------------------------------------------------
 */

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
RA8_INTERNAL
RA8_INTERNAL static uint8_t internal_png_paeth(uint8_t a, uint8_t b, uint8_t c)
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
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_png_unfilter(uint8_t        filter,
                                                    uint8_t*       row,
                                                    const uint8_t* prev,
                                                    uint32_t       nbytes,
                                                    uint8_t        bpp)
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
        row[i]          = (uint8_t)(row[i] + internal_png_paeth(a, prev[i], c));
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
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_png_translate(ra8_png_state_t* st, const uint8_t* row)
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
      const uint8_t  g   = row[(size_t)x * (size_t)k_ra8_png_ch_2];
      const uint8_t  a   = row[((size_t)x * (size_t)k_ra8_png_ch_2) + 1U];
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
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t
internal_png_consume_rows(ra8_png_state_t* st, const uint8_t* src, uint32_t avail)
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
    ra8_err_t      err =
      internal_png_unfilter(st->rowbuf[0], &st->rowbuf[1], st->prevrow, nbytes, st->src_ch);
    if (err != k_ra8_ok) {
      return err;
    }
    (void)memcpy(st->prevrow, &st->rowbuf[1], (size_t)nbytes);
    err = internal_png_translate(st, &st->rowbuf[1]);
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
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_png_bind_geometry(ra8_png_state_t* st, ra8_jof_bump_t* bump)
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
  /* The bump allocator returns 8-byte-aligned void* carves, which satisfies
   * the inflate context's alignment. */
  st->inflator = (tinfl_decompressor*)priv_jof_bump_take(bump, sizeof(tinfl_decompressor));
  st->ring     = priv_jof_bump_take(bump, (size_t)k_ra8_png_ring_bytes);
  st->inbuf    = priv_jof_bump_take(bump, (size_t)k_ra8_png_inbuf_bytes);
  st->rowbuf   = priv_jof_bump_take(bump, (size_t)st->rowlen);
  st->prevrow  = priv_jof_bump_take(bump, (size_t)(st->rowlen - 1U));
  st->xlat     = priv_jof_bump_take(bump, (size_t)st->w * (size_t)st->dst_ch);
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
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_png_refill_input(ra8_png_state_t* st, uint32_t* out_avail)
{
  *out_avail = 0U;
  for (uint32_t guard = 0U; guard < (uint32_t)k_ra8_png_max_chunks; guard++) {
    if (st->idat_rem > 0U) {
      const uint32_t  take = (st->idat_rem < (uint32_t)k_ra8_png_inbuf_bytes)
                               ? st->idat_rem
                               : (uint32_t)k_ra8_png_inbuf_bytes;
      const ra8_err_t err  = priv_jof_png_pull_exact(st, st->inbuf, take);
      if (err != k_ra8_ok) {
        return err;
      }
      st->idat_rem -= take;
      *out_avail = take;
      return k_ra8_ok;
    }
    /* Current IDAT exhausted: skip its CRC, look at the next chunk. */
    ra8_err_t err = priv_jof_png_skip(st, (uint32_t)k_ra8_png_crc_bytes);
    if (err != k_ra8_ok) {
      return err;
    }
    uint32_t len  = 0U;
    uint32_t type = 0U;
    err           = priv_jof_png_chunk_hdr(st, &len, &type);
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
 * @struct ra8_png_iter_t
 * @brief Inflate-loop cursor: compressed-input window + progress guard.
 *
 * @invariant `in_pos <= in_avail` at all times.
 */
typedef struct {
  uint32_t in_avail; /**< Valid compressed bytes in `inbuf`.    */
  uint32_t in_pos;   /**< Consumed compressed bytes.            */
  uint8_t  stalls;   /**< Consecutive zero-progress iterations. */
  uint8_t  done;     /**< 1 once tinfl reported stream end.     */
} ra8_png_iter_t;

/**
 * @brief One inflate iteration: refill, tinfl, progress guard, drain rows.
 * @details The ring is tinfl's LZ dictionary; each call produces into the
 *          contiguous run `[ring_wr, ring_end)` which drains into the
 *          scanline assembler before the write offset wraps. Two
 *          consecutive zero-progress iterations (or any stall after the
 *          source ended) abort as a truncated/hostile stream.
 * @param[in,out] st Decoder state.
 * @param[in,out] it Loop cursor (input window + guards).
 * @return Result code.
 * @retval k_ra8_ok                    Progress made (or clean stream end).
 * @retval k_ra8_err_protocol_error    Corrupt deflate / no progress.
 * @retval k_ra8_err_validation_failed Row-stream inconsistency.
 * @retval other                       Propagated from pull / the hooks.
 * @pre `internal_png_bind_geometry()` succeeded.
 * @pre @p it was zero-initialised before the first call.
 * @post `it->done` is set once the zlib stream terminates.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_png_inflate_step(ra8_png_state_t* st, ra8_png_iter_t* it)
{
  // mcdc-deactivated: refill-first loop structure; entering with a drained window after the source ended requires the source-ending iteration to return >= TINFL_STATUS_DONE without finishing, i.e. tinfl parked output mid-flush at the exact call the source ended -- for every constructible stream that call either completes (DONE exits the loop) or fails closed at the status check, so the (drained, source-done) entry cannot be flipped independently.
  if ((it->in_pos == it->in_avail) && (st->source_done == 0U)) {
    const ra8_err_t err = internal_png_refill_input(st, &it->in_avail);
    if (err != k_ra8_ok) {
      return err;
    }
    it->in_pos = 0U;
  }
  size_t             in_sz  = (size_t)(it->in_avail - it->in_pos);
  size_t             out_sz = (size_t)((uint32_t)k_ra8_png_ring_bytes - st->ring_wr);
  const mz_uint      flags  = (mz_uint)TINFL_FLAG_PARSE_ZLIB_HEADER |
                              ((st->source_done == 0U) ? (mz_uint)TINFL_FLAG_HAS_MORE_INPUT : 0U);
  const tinfl_status status = tinfl_decompress(st->inflator,
                                               &st->inbuf[it->in_pos],
                                               &in_sz,
                                               st->ring,
                                               &st->ring[st->ring_wr],
                                               &out_sz,
                                               flags);
  if (status < TINFL_STATUS_DONE) {
    return k_ra8_err_protocol_error; /* corrupt deflate / bad zlib header */
  }
  // mcdc-deactivated: zero-progress stall guard, defense-in-depth against a decompressor that spins without failing; tinfl never returns >= TINFL_STATUS_DONE with neither input consumed nor output produced (a starved mid-stream call without HAS_MORE_INPUT fails closed at the status check above, and the refill-first structure guarantees input is present otherwise), so the stall arm is not constructible from any source stream.
  if ((in_sz == 0U) && (out_sz == 0U)) {
    it->stalls++;
    // mcdc-deactivated: inner arm of the non-constructible stall guard above (same rationale); kept so a hypothetical spinning decompressor aborts after one repeat instead of looping.
    if ((it->stalls > 1U) || (st->source_done != 0U)) {
      return k_ra8_err_protocol_error; /* truncated stream / no progress */
    }
  } else {
    it->stalls = 0U;
  }
  if (out_sz > 0U) {
    const ra8_err_t err = internal_png_consume_rows(st, &st->ring[st->ring_wr], (uint32_t)out_sz);
    if (err != k_ra8_ok) {
      return err;
    }
    st->ring_wr = (st->ring_wr + (uint32_t)out_sz) & ((uint32_t)k_ra8_png_ring_bytes - 1U);
  }
  it->in_pos += (uint32_t)in_sz;
  it->done = (status == TINFL_STATUS_DONE) ? 1U : 0U;
  return k_ra8_ok;
}

/**
 * @brief Inflate the whole IDAT stream, emitting every scanline.
 * @details Drives ::internal_png_inflate_step until the zlib stream terminates, then
 *          applies the strict termination checks: every declared row
 *          arrived, no partial scanline remains, and no compressed bytes
 *          trail the stream inside the IDAT chunks.
 * @param[in,out] st        Decoder state.
 * @param[in]     first_len Payload length of the first IDAT chunk.
 * @return Result code.
 * @retval k_ra8_ok                    All rows emitted; stream ended clean.
 * @retval k_ra8_err_protocol_error    Corrupt / truncated deflate stream.
 * @retval k_ra8_err_validation_failed Row-count / trailing-byte mismatch.
 * @retval other                       Propagated from pull / the hooks.
 * @pre `internal_png_bind_geometry()` succeeded.
 * @pre The source sits at the first IDAT's payload.
 * @post On success `rows_done == h` and the pending chunk is parked.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t internal_png_inflate_idat(ra8_png_state_t* st, uint32_t first_len)
{
  st->idat_rem      = first_len;
  ra8_png_iter_t it = {};
  while (it.done == 0U) {
    const ra8_err_t err = internal_png_inflate_step(st, &it);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  /* Strict termination: every declared row arrived, nothing extra. */
  if ((st->rows_done != st->h) || (st->rowfill != 0U)) {
    return k_ra8_err_validation_failed;
  }
  if ((it.in_pos != it.in_avail) || (st->idat_rem != 0U)) {
    return k_ra8_err_validation_failed; /* trailing garbage inside IDAT */
  }
  return k_ra8_ok;
}

/* ---------------------------------------------------------------------------
 * Entry point.
 * ---------------------------------------------------------------------------
 */

/**
 * @brief The IDAT arm: bind geometry, inflate the stream, finish the walk.
 * @details Runs once, at the first IDAT chunk; every subsequent IDAT is
 *          consumed inside the inflate phase.
 * @param[in,out] st   Decoder state.
 * @param[in,out] bump Work-arena allocator for the pixel-path carves.
 * @param[in]     len  Payload length of the first IDAT chunk.
 * @return Result code.
 * @retval k_ra8_ok Every row emitted and the datastream consumed to IEND.
 * @retval other    Propagated from geometry / inflate / the chunk walk.
 * @pre The IHDR (and any PLTE/tRNS) have been parsed.
 * @pre @p bump has the PNG carve set available.
 * @post On success the whole PNG has been consumed.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t
internal_png_run_idat(ra8_png_state_t* st, ra8_jof_bump_t* bump, uint32_t len)
{
  ra8_err_t err = internal_png_bind_geometry(st, bump);
  if (err != k_ra8_ok) {
    return err;
  }
  err = internal_png_inflate_idat(st, len);
  if (err != k_ra8_ok) {
    return err;
  }
  return priv_jof_png_finish(st);
}

/**
 * @brief Walk the chunk stream: pre-IDAT chunks, then the IDAT arm.
 * @details Bounded by the shared chunk budget (NASA Rule 2); a stream that
 *          never reaches an IDAT within it is rejected as hostile.
 * @param[in,out] st    Decoder state (prologue already parsed).
 * @param[in,out] bump  Work-arena allocator for the pixel-path carves.
 * @param[in]     max_w Fail-closed width cap (for the prologue).
 * @param[in]     max_h Fail-closed height cap (for the prologue).
 * @return Result code.
 * @retval k_ra8_ok                 Every row emitted; stream consumed.
 * @retval k_ra8_err_protocol_error Structure error / chunk budget spent.
 * @retval other                    Propagated from the chunk / IDAT arms.
 * @pre The callbacks are bound in @p st.
 * @pre The source is positioned at byte 0 of the PNG stream.
 * @post On success the whole PNG has been consumed.
 * @post On error the decode aborts.
 * @note Not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_INTERNAL static ra8_err_t
internal_png_walk_chunks(ra8_png_state_t* st, ra8_jof_bump_t* bump, uint16_t max_w, uint16_t max_h)
{
  ra8_err_t err = priv_jof_png_prologue(st, max_w, max_h);
  if (err != k_ra8_ok) {
    return err;
  }
  for (uint32_t guard = 0U; guard < (uint32_t)k_ra8_png_max_chunks; guard++) {
    uint32_t len  = 0U;
    uint32_t type = 0U;
    err           = priv_jof_png_chunk_hdr(st, &len, &type);
    if (err != k_ra8_ok) {
      return err;
    }
    if (type == (uint32_t)k_ra8_png_type_idat) {
      return internal_png_run_idat(st, bump, len);
    }
    err = priv_jof_png_pre_idat(st, len, type);
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_err_protocol_error; /* chunk budget exhausted (hostile) */
}

RA8_PRIV ra8_err_t priv_jof_png_rows(ra8_jof_pull_fn pull,
                                     void*           pull_ctx,
                                     ra8_jof_bump_t* bump,
                                     uint16_t        max_w,
                                     uint16_t        max_h,
                                     ra8_jof_geom_fn on_geom,
                                     ra8_jof_rows_fn on_rows,
                                     void*           cb_ctx)
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
  return internal_png_walk_chunks(st, bump, max_w, max_h);
}
