/**
 * @file ra8_jpeg_sw_decode.c
 * @brief Pure-software baseline JPEG decoder: marker parser and scan.
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Implements the marker-segment parser (DQT / DHT / SOF0 / SOS), the
 * per-MCU entropy-decode + IDCT + colour-conversion scan loop, and the
 * `ra8_jpeg_sw_decode()` public API for baseline (8-bit, sequential,
 * Huffman) JPEG streams in the YCbCr 4:2:0, YCbCr 4:4:4 and grayscale
 * layouts. The reference codec is the C99 reformulation of "TJpgDec"
 * by ChaN, re-implemented from scratch in this project's style; no
 * third-party code is copied.
 *
 * This is the decoder-driver half of the software JPEG codec; the
 * shared entropy/DSP primitives (bit reader, Huffman tables, inverse
 * DCT, colour conversion) and the `ra8_jpeg_sw_get_dimensions()` public
 * API live in `ra8_jpeg_sw.c`, and every cross-unit symbol lives in
 * `ra8_jpeg_sw_internal.h`.
 *
 * Spec citations are tagged `T.81 sec X.Y "..."` and refer to
 * ITU-T Recommendation T.81 (1992) | ISO/IEC 10918-1.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_jpeg_sw.h"
#include "ra8_jpeg_sw_internal.h"
#include "ra8_log.h"

/** @brief Component log tag. */
static const char* s_tag = "JPEG_SW";

/* ------------------------------------------------------------------ */
/* Decoder */
/* ------------------------------------------------------------------ */

/* The decoder context (`ra8_jpeg_dec_ctx_t`) and the marker/scan
 * primitives below are declared in `ra8_jpeg_sw_internal.h` so the
 * streaming driver (`ra8_jpeg_sw_stream.c`) can reuse them over its
 * sliding window; this unit keeps their definitions plus the
 * whole-buffer `ra8_jpeg_sw_decode()` driver. */

/** @brief Implementation of `ra8_jpeg_sw_priv_skip_segment()` -- bounds-checked cursor hop. */
RA8_PRIV ra8_err_t ra8_jpeg_sw_priv_skip_segment(ra8_jpeg_dec_ctx_t* d)
{
  if (d->cursor + 2U > d->src_len) {
    return k_ra8_err_protocol_error;
  }
  uint16_t len = read_be16(&d->src[d->cursor]);
  if (len < 2U || (uint32_t)len > d->src_len - d->cursor) {
    return k_ra8_err_protocol_error;
  }
  d->cursor += len;
  return k_ra8_ok;
}

/** @brief Implementation of `ra8_jpeg_sw_priv_parse_dqt()` -- T.81 B.2.4.1 de-zigzag parse. */
RA8_PRIV ra8_err_t ra8_jpeg_sw_priv_parse_dqt(ra8_jpeg_dec_ctx_t* d)
{
  if (d->cursor + 2U > d->src_len) {
    return k_ra8_err_protocol_error;
  }
  uint16_t len = read_be16(&d->src[d->cursor]);
  if (len < 2U || (uint32_t)len > d->src_len - d->cursor) {
    return k_ra8_err_protocol_error;
  }
  uint32_t end = d->cursor + len;
  d->cursor += 2U;
  while (d->cursor < end) {
    uint8_t pq_tq = d->src[d->cursor];
    d->cursor++;
    uint8_t pq = (uint8_t)(pq_tq >> k_ra8_jpeg_nibble_shift);
    uint8_t tq = (uint8_t)(pq_tq & k_ra8_jpeg_nibble_mask);
    if (tq >= (uint8_t)k_ra8_jpeg_quant_tabs || pq != 0U) {
      return k_ra8_err_not_supported; /* 16-bit precision not supported. */
    }
    if (d->cursor + (uint32_t)k_ra8_jpeg_block_size > end) {
      return k_ra8_err_protocol_error;
    }
    for (uint8_t i = 0U; i < (uint8_t)k_ra8_jpeg_block_size; i++) {
      d->qtab[tq][s_zigzag[i]] = d->src[d->cursor];
      d->cursor++;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Parse one (TcTh, BITS, HUFFVAL) table out of a DHT segment.
 *
 * @details
 * Consumes exactly one Huffman-table record starting at `d->cursor`
 * (T.81 sec B.2.4.2): the TcTh selector byte, the 16-entry BITS list
 * and the HUFFVAL symbol list, then canonical-builds the table via
 * `ra8_jpeg_sw_htab_build()`. Called in a loop by
 * `ra8_jpeg_sw_priv_parse_dht()` until the segment is exhausted.
 *
 * @param[in,out] d   Decoder context (cursor advances; table written).
 * @param[in]     end One-past-the-end offset of the DHT segment.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 One table parsed and built.
 * @retval k_ra8_err_protocol_error Truncated record or symbol overflow.
 * @retval k_ra8_err_not_supported  Table class / id out of range.
 *
 * @pre `d->cursor < end` (caller's loop condition).
 * @pre `end <= d->src_len` (validated by the caller).
 * @post On success `d->cursor` sits at the next record (or `end`).
 * @post On error the decode aborts; table state is partial.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t dec_parse_dht_one(ra8_jpeg_dec_ctx_t* d, uint32_t end)
{
  uint8_t tc_th = d->src[d->cursor];
  d->cursor++;
  uint8_t tc = (uint8_t)(tc_th >> k_ra8_jpeg_nibble_shift);
  uint8_t th = (uint8_t)(tc_th & k_ra8_jpeg_nibble_mask);
  if (tc >= (uint8_t)k_ra8_jpeg_huff_classes || th >= (uint8_t)k_ra8_jpeg_huff_ids) {
    return k_ra8_err_not_supported;
  }
  if (d->cursor + (uint32_t)k_ra8_jpeg_huff_lengths > end) {
    return k_ra8_err_protocol_error;
  }
  ra8_jpeg_htab_t* h     = (tc == 0U) ? &d->hdc[th] : &d->hac[th];
  uint16_t         total = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_jpeg_huff_lengths; i++) {
    h->bits[i] = d->src[d->cursor];
    d->cursor++;
    total = (uint16_t)(total + h->bits[i]);
  }
  if (total > (uint16_t)k_ra8_jpeg_huff_max) {
    return k_ra8_err_protocol_error;
  }
  if (d->cursor + total > end) {
    return k_ra8_err_protocol_error;
  }
  for (uint16_t i = 0U; i < total; i++) {
    h->vals[i] = d->src[d->cursor];
    d->cursor++;
  }
  ra8_jpeg_sw_htab_build(h);
  return k_ra8_ok;
}

/** @brief Implementation of `ra8_jpeg_sw_priv_parse_dht()` -- T.81 B.2.4.2 record loop. */
RA8_PRIV ra8_err_t ra8_jpeg_sw_priv_parse_dht(ra8_jpeg_dec_ctx_t* d)
{
  if (d->cursor + 2U > d->src_len) {
    return k_ra8_err_protocol_error;
  }
  uint16_t len = read_be16(&d->src[d->cursor]);
  if (len < 2U || (uint32_t)len > d->src_len - d->cursor) {
    return k_ra8_err_protocol_error;
  }
  uint32_t end = d->cursor + len;
  d->cursor += 2U;
  while (d->cursor < end) {
    ra8_err_t e = dec_parse_dht_one(d, end);
    if (e != k_ra8_ok) {
      return e;
    }
  }
  return k_ra8_ok;
}

/**
 * @brief Read the per-component id/sampling/quant fields of an SOF0.
 *
 * @details
 * Consumes `d->ncomp` three-byte component records (T.81 sec B.2.2:
 * Ci, HiVi, Tqi) starting at `*s`, filling the per-component arrays
 * and folding the running `hmax`/`vmax` maxima exactly as the previous
 * monolithic `ra8_jpeg_sw_priv_parse_sof0()` body did.
 *
 * @param[in,out] d Decoder context (component tables + hmax/vmax written).
 * @param[in,out] s Byte cursor into the SOF0 payload (advances 3/comp).
 *
 * @pre `d->ncomp` is 1 or 3 (validated by the caller).
 * @pre The `3 * ncomp` record bytes are inside the segment (caller-checked).
 * @post `d->comp_*[0..ncomp-1]` and `d->hmax`/`d->vmax` are populated.
 * @post `*s` advanced past the last component record.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static void dec_parse_sof0_components(ra8_jpeg_dec_ctx_t* d, uint32_t* s)
{
  d->hmax = 0U;
  d->vmax = 0U;
  for (uint8_t i = 0U; i < d->ncomp; i++) {
    d->comp_id[i] = d->src[*s];
    (*s)++;
    uint8_t hv = d->src[*s];
    (*s)++;
    d->comp_h[i]   = (uint8_t)(hv >> k_ra8_jpeg_nibble_shift);
    d->comp_v[i]   = (uint8_t)(hv & k_ra8_jpeg_nibble_mask);
    d->comp_qid[i] = d->src[*s];
    (*s)++;
    if (d->comp_h[i] > d->hmax) {
      d->hmax = d->comp_h[i];
    }
    if (d->comp_v[i] > d->vmax) {
      d->vmax = d->comp_v[i];
    }
  }
}

/**
 * @brief Accept only the 4:4:4 and 4:2:0 3-component chroma layouts.
 *
 * @details
 * Applies the codec's format envelope to the sampling factors parsed
 * from SOF0: for 3-component streams only 4:4:4 (all 1x1) and 4:2:0
 * (luma 2x2, both chromas 1x1) are decodable; grayscale streams pass
 * unconditionally.
 *
 * @param[in] d Decoder context with `ncomp`/`hmax`/`vmax`/`comp_*` set.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Layout is 4:4:4, 4:2:0 or grayscale.
 * @retval k_ra8_err_not_supported Any other chroma layout.
 *
 * @pre `d` is non-NULL (module-internal call chain).
 * @pre `dec_parse_sof0_components()` ran for this frame.
 * @post No decoder state is mutated.
 * @post Return value fully determines whether the scan may proceed.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t dec_check_chroma_layout(const ra8_jpeg_dec_ctx_t* d)
{
  /* Only 4:4:4 (1,1,1) and 4:2:0 (2,1,1) accepted. */
  if (d->ncomp == 3U) {
    bool is_444 = (d->hmax == 1U && d->vmax == 1U);
    bool is_420 = (d->hmax == 2U && d->vmax == 2U && d->comp_h[1] == 1U && d->comp_v[1] == 1U &&
                   d->comp_h[2] == 1U && d->comp_v[2] == 1U);
    if (!is_444 && !is_420) {
      return k_ra8_err_not_supported;
    }
  }
  return k_ra8_ok;
}

/** @brief Implementation of `ra8_jpeg_sw_priv_parse_sof0()` -- T.81 B.2.2 frame header. */
RA8_PRIV ra8_err_t ra8_jpeg_sw_priv_parse_sof0(ra8_jpeg_dec_ctx_t* d)
{
  if (d->cursor + 2U > d->src_len) {
    return k_ra8_err_protocol_error;
  }
  uint16_t len = read_be16(&d->src[d->cursor]);
  if (len < 8U || (uint32_t)len > d->src_len - d->cursor) {
    return k_ra8_err_protocol_error;
  }
  uint32_t s         = d->cursor + 2U;
  uint8_t  precision = d->src[s];
  s++;
  if (precision != 8U) {
    return k_ra8_err_not_supported;
  }
  d->height = read_be16(&d->src[s]);
  s += 2U;
  d->width = read_be16(&d->src[s]);
  s += 2U;
  d->ncomp = d->src[s];
  s++;
  if (d->ncomp != 1U && d->ncomp != 3U) {
    return k_ra8_err_not_supported;
  }
  if (((uint32_t)d->ncomp * 3U) + (s - d->cursor) > (uint32_t)len) {
    return k_ra8_err_protocol_error;
  }
  dec_parse_sof0_components(d, &s);
  d->cursor += len;
  return dec_check_chroma_layout(d);
}

/** @brief Implementation of `ra8_jpeg_sw_priv_parse_sos()` -- T.81 B.2.3 selector binding. */
RA8_PRIV ra8_err_t ra8_jpeg_sw_priv_parse_sos(ra8_jpeg_dec_ctx_t* d)
{
  if (d->cursor + 2U > d->src_len) {
    return k_ra8_err_protocol_error;
  }
  uint16_t len = read_be16(&d->src[d->cursor]);
  if (len < 6U || (uint32_t)len > d->src_len - d->cursor) {
    return k_ra8_err_protocol_error;
  }
  uint32_t s  = d->cursor + 2U;
  uint8_t  ns = d->src[s];
  s++;
  if (ns != d->ncomp) {
    return k_ra8_err_not_supported;
  }
  for (uint8_t i = 0U; i < ns; i++) {
    uint8_t cs = d->src[s];
    s++;
    uint8_t tdta = d->src[s];
    s++;
    uint8_t idx = i;
    for (uint8_t j = 0U; j < d->ncomp; j++) {
      if (d->comp_id[j] == cs) {
        idx = j;
        break;
      }
    }
    d->comp_dc_id[idx] = (uint8_t)(tdta >> k_ra8_jpeg_nibble_shift);
    d->comp_ac_id[idx] = (uint8_t)(tdta & k_ra8_jpeg_nibble_mask);
    if (d->comp_dc_id[idx] >= (uint8_t)k_ra8_jpeg_huff_ids ||
        d->comp_ac_id[idx] >= (uint8_t)k_ra8_jpeg_huff_ids) {
      return k_ra8_err_not_supported;
    }
  }
  /* Skip Ss/Se/AhAl (3 bytes). */
  d->cursor += len;
  return k_ra8_ok;
}

/**
 * @brief Run-length decode the 63 AC coefficients of one block.
 *
 * @details
 * Implements the T.81 sec F.2.2.2 AC decode loop: each Huffman symbol
 * packs a zero-run nibble (RRRR) and a magnitude-bit count (SSSS);
 * ZRL (0xF0) skips 16 zeros, SSSS == 0 with RRRR == 0 is EOB. Each
 * non-zero coefficient is sign-extended, dequantized through the
 * component's quant table and de-zigzagged into `outblk`.
 *
 * @param[in,out] d      Decoder context (read-only tables used).
 * @param[in,out] br     Bit reader over the entropy-coded segment.
 * @param[in]     ci     Component index (0 = luma).
 * @param[out]    outblk 64-entry coefficient block (AC slots written).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 AC coefficients decoded (or EOB hit).
 * @retval k_ra8_err_protocol_error Stream underflow, illegal symbol, or
 *                                  zig-zag index overflow.
 *
 * @pre `outblk` was zero-filled and DC-populated by the caller.
 * @pre `d`'s AC Huffman/quant tables for `ci` were parsed.
 * @post On success every non-zero AC coefficient is dequantized in place.
 * @post On error the scan aborts.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t
dec_block_ac(ra8_jpeg_dec_ctx_t* d, ra8_jpeg_bitreader_t* br, uint8_t ci, int32_t* outblk)
{
  uint8_t k = 1U;
  while (k < (uint8_t)k_ra8_jpeg_block_size) {
    int32_t rs = ra8_jpeg_sw_htab_decode(br, &d->hac[d->comp_ac_id[ci]]);
    if (rs < 0) {
      return k_ra8_err_protocol_error;
    }
    uint8_t rrrr = (uint8_t)(rs >> k_ra8_jpeg_nibble_shift);
    uint8_t ssss = (uint8_t)(rs & k_ra8_jpeg_nibble_mask);
    if (ssss == 0U) {
      if (rrrr == k_jpeg_nibble_mask) {
        k = (uint8_t)(k + 16U); /* ZRL: 16 zero coeffs. */
        continue;
      }
      break; /* EOB. */
    }
    k = (uint8_t)(k + rrrr);
    if (k >= (uint8_t)k_ra8_jpeg_block_size) {
      return k_ra8_err_protocol_error;
    }
    int32_t v = ra8_jpeg_sw_br_get_bits(br, ssss);
    if (v < 0) {
      return k_ra8_err_protocol_error;
    }
    v                   = ra8_jpeg_sw_huff_extend(v, ssss);
    outblk[s_zigzag[k]] = v * (int32_t)d->qtab[d->comp_qid[ci]][s_zigzag[k]];
    k++;
  }
  return k_ra8_ok;
}

/** @brief Implementation of `ra8_jpeg_sw_priv_block()` -- T.81 F.2.2 DC diff + AC run-length. */
RA8_PRIV ra8_err_t ra8_jpeg_sw_priv_block(ra8_jpeg_dec_ctx_t*   d,
                                          ra8_jpeg_bitreader_t* br,
                                          uint8_t               ci,
                                          int32_t*              outblk)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_jpeg_block_size; i++) {
    outblk[i] = 0;
  }
  /* DC. */
  int32_t t = ra8_jpeg_sw_htab_decode(br, &d->hdc[d->comp_dc_id[ci]]);
  if (t < 0) {
    return k_ra8_err_protocol_error;
  }
  int32_t r = ra8_jpeg_sw_br_get_bits(br, (uint8_t)t);
  /* mcdc-deactivated: ra8_jpeg_sw_br_get_bits returns -1 only when the bitstream
   * is exhausted; reaching this branch with t != 0 requires a
   * truncated entropy-coded segment after every parser stage has
   * succeeded -- the public-API contract (well-formed JFIF stream
   * with EOI) makes this branch unreachable. Defensive guard for
   * fault injection. */
  // mcdc-deactivated: ra8_jpeg_sw_priv_block ra8_jpeg_sw_br_get_bits exhaustion guard; well-formed JFIF streams (public-API contract) cannot exhaust the bitstream mid-coefficient with t != 0 because every parser stage upstream has validated the segment-length budget.
  if (r < 0 && t != 0) {
    return k_ra8_err_protocol_error;
  }
  int32_t diff = ra8_jpeg_sw_huff_extend(r, (uint8_t)t);
  d->comp_dc_pred[ci] += diff;
  outblk[0] = d->comp_dc_pred[ci] * (int32_t)d->qtab[d->comp_qid[ci]][0];

  /* AC. */
  return dec_block_ac(d, br, ci, outblk);
}

/* ------------------------------------------------------------------ */
/* Public API: decode */
/* ------------------------------------------------------------------ */

/** @brief Implementation of `ra8_jpeg_sw_priv_idct_into()` -- IDCT + level shift + clamp. */
RA8_PRIV void ra8_jpeg_sw_priv_idct_into(int32_t* coeffs, uint8_t* tile)
{
  ra8_jpeg_sw_idct8x8(coeffs);
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_jpeg_block_size; i++) {
    int32_t v = coeffs[i] + (int32_t)k_ra8_jpeg_level_offset;
    tile[i]   = clamp_u8(v);
  }
}

/**
 * @brief Copy one reconstructed 8x8 luma block into its Y-tile slot.
 *
 * @details
 * Writes the 64 samples of `blk` into the (bx, by) sub-rectangle of
 * the MCU's luma tile, whose rows are `mcu_w_px` samples wide, so the
 * MCU pixel-emit loop can address the tile linearly.
 *
 * @param[in]  blk      64-byte reconstructed luma block (row-major).
 * @param[out] y_tile   Destination MCU luma tile.
 * @param[in]  bx       Block column inside the MCU (0..hmax-1).
 * @param[in]  by       Block row inside the MCU (0..vmax-1).
 * @param[in]  mcu_w_px MCU width in pixels (tile row stride).
 *
 * @pre `blk` holds 64 valid samples.
 * @pre `y_tile` covers `mcu_w_px * 8 * (by + 1)` bytes.
 * @post The (bx, by) sub-rectangle of `y_tile` is populated.
 * @post No other tile bytes are touched.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static void dec_copy_block_to_tile(const uint8_t* blk,
                                   uint8_t*       y_tile,
                                   uint8_t        bx,
                                   uint8_t        by,
                                   uint16_t       mcu_w_px)
{
  for (uint8_t r = 0U; r < (uint8_t)k_ra8_jpeg_block_dim; r++) {
    for (uint8_t c = 0U; c < (uint8_t)k_ra8_jpeg_block_dim; c++) {
      uint16_t ty                  = (uint16_t)((by * (uint16_t)k_ra8_jpeg_block_dim) + r);
      uint16_t tx                  = (uint16_t)((bx * (uint16_t)k_ra8_jpeg_block_dim) + c);
      y_tile[(ty * mcu_w_px) + tx] = blk[(r * (uint8_t)k_ra8_jpeg_block_dim) + c];
    }
  }
}

/** @brief Implementation of `ra8_jpeg_sw_priv_mcu_y()` -- hmax*vmax luma block loop. */
RA8_PRIV ra8_err_t ra8_jpeg_sw_priv_mcu_y(ra8_jpeg_dec_ctx_t*   d,
                                          ra8_jpeg_bitreader_t* br,
                                          uint8_t*              y_tile,
                                          uint16_t              mcu_w_px)
{
  int32_t coeffs[(uint32_t)k_ra8_jpeg_block_size];
  for (uint8_t by = 0U; by < d->vmax; by++) {
    for (uint8_t bx = 0U; bx < d->hmax; bx++) {
      ra8_err_t e = ra8_jpeg_sw_priv_block(d, br, 0U, coeffs);
      if (e != k_ra8_ok) {
        return e;
      }
      uint8_t blk[(uint32_t)k_ra8_jpeg_block_size];
      ra8_jpeg_sw_priv_idct_into(coeffs, blk);
      dec_copy_block_to_tile(blk, y_tile, bx, by, mcu_w_px);
    }
  }
  return k_ra8_ok;
}

/** @brief Implementation of `ra8_jpeg_sw_priv_mcu_chroma()` -- one Cb then one Cr block. */
RA8_PRIV ra8_err_t ra8_jpeg_sw_priv_mcu_chroma(ra8_jpeg_dec_ctx_t*   d,
                                               ra8_jpeg_bitreader_t* br,
                                               uint8_t*              cb_tile,
                                               uint8_t*              cr_tile)
{
  int32_t   coeffs[(uint32_t)k_ra8_jpeg_block_size];
  ra8_err_t e = ra8_jpeg_sw_priv_block(d, br, 1U, coeffs);
  if (e != k_ra8_ok) {
    return e;
  }
  ra8_jpeg_sw_priv_idct_into(coeffs, cb_tile);
  e = ra8_jpeg_sw_priv_block(d, br, 2U, coeffs);
  if (e != k_ra8_ok) {
    return e;
  }
  ra8_jpeg_sw_priv_idct_into(coeffs, cr_tile);
  return k_ra8_ok;
}

/**
 * @brief Convert one MCU's reconstructed YCbCr tiles into output RGB pixels.
 *
 * @details
 * Walks every pixel of the MCU at output position (mx,my), pulling the
 * matching Y sample from `y_tile` and the (sub-sampled) Cb/Cr samples
 * from `cb_tile`/`cr_tile` according to the stream's hmax/vmax. For
 * grayscale streams (ncomp == 1) the chroma samples default to the
 * level-offset (128) so the BT.601 transform produces R==G==B==Y.
 *
 * @param[in]  d        Decoder context (provides hmax, vmax, width/height).
 * @param[in]  y_tile   mcu_w_px-stride buffer of Y samples for this MCU.
 * @param[in]  cb_tile  64-byte Cb tile (ignored when d->ncomp != 3).
 * @param[in]  cr_tile  64-byte Cr tile (ignored when d->ncomp != 3).
 * @param[in]  mx       MCU column index in the output image.
 * @param[in]  my       MCU row index in the output image.
 * @param[in]  mcu_w_px MCU width in pixels.
 * @param[in]  mcu_h_px MCU height in pixels.
 * @param[out] out_buf  Destination RGB888 image buffer.
 *
 * @return None.
 *
 * @pre `out_buf` has space for d->width * d->height * 3 bytes.
 * @pre `mcu_w_px > 0` and `mcu_h_px > 0`.
 * @post `out_buf` updated only inside the visible region of this MCU.
 * @post No JPEG-stream state is modified.
 *
 * @note Not thread-safe; caller serializes via decoder context.
 * @since 0.1.0
 */
static void dec_emit_mcu_rgb(const ra8_jpeg_dec_ctx_t* d,
                             const uint8_t*            y_tile,
                             const uint8_t*            cb_tile,
                             const uint8_t*            cr_tile,
                             uint16_t                  mx,
                             uint16_t                  my,
                             uint16_t                  mcu_w_px,
                             uint16_t                  mcu_h_px,
                             uint8_t*                  out_buf)
{
  for (uint16_t r = 0U; r < mcu_h_px; r++) {
    uint16_t py = (uint16_t)((my * mcu_h_px) + r);
    if (py >= d->height) {
      break;
    }
    for (uint16_t c = 0U; c < mcu_w_px; c++) {
      uint16_t px = (uint16_t)((mx * mcu_w_px) + c);
      if (px >= d->width) {
        break;
      }
      int32_t y = (int32_t)y_tile[(r * mcu_w_px) + c];
      int32_t cb;
      int32_t cr;
      if (d->ncomp == 3U) {
        uint16_t cr_x = (uint16_t)(c / d->hmax);
        uint16_t cr_y = (uint16_t)(r / d->vmax);
        cb            = (int32_t)cb_tile[(cr_y * (uint16_t)k_ra8_jpeg_block_dim) + cr_x];
        cr            = (int32_t)cr_tile[(cr_y * (uint16_t)k_ra8_jpeg_block_dim) + cr_x];
      } else {
        cb = (int32_t)k_ra8_jpeg_level_offset;
        cr = (int32_t)k_ra8_jpeg_level_offset;
      }
      uint32_t idx =
        (((uint32_t)py * (uint32_t)d->width) + (uint32_t)px) * (uint32_t)k_ra8_jpeg_rgb_components;
      ra8_jpeg_sw_ycc_to_rgb(y, cb, cr, &out_buf[idx], &out_buf[idx + 1U], &out_buf[idx + 2U]);
    }
  }
}

/**
 * @brief Decode one MCU (luma + optional chroma) and emit its RGB pixels.
 *
 * @details
 * Loop body of `dec_decode_scan()`: entropy-decodes the MCU's
 * `hmax*vmax` luma blocks into `y_tile`, the Cb/Cr pair for
 * 3-component streams, and converts the reconstructed tiles into
 * output RGB at MCU position (mx, my).
 *
 * @param[in,out] d        Decoder context (DC predictors mutate).
 * @param[in,out] br       Bit reader over the entropy-coded segment.
 * @param[out]    y_tile   Scratch luma tile (mcu_w_px * mcu_h_px bytes).
 * @param[out]    cb_tile  Scratch 64-byte Cb tile.
 * @param[out]    cr_tile  Scratch 64-byte Cr tile.
 * @param[in]     mx       MCU column index in the output image.
 * @param[in]     my       MCU row index in the output image.
 * @param[in]     mcu_w_px MCU width in pixels.
 * @param[in]     mcu_h_px MCU height in pixels.
 * @param[out]    out_buf  Destination RGB888 image buffer.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 MCU decoded and emitted.
 * @retval k_ra8_err_protocol_error Entropy-stream error in any block.
 *
 * @pre All tile buffers are sized for the stream's sampling layout.
 * @pre `out_buf` covers the full decoded image (caller-checked).
 * @post On success the MCU's visible pixels are written to `out_buf`.
 * @post On error the scan aborts; already-emitted pixels stay valid.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t dec_decode_mcu(ra8_jpeg_dec_ctx_t*   d,
                                ra8_jpeg_bitreader_t* br,
                                uint8_t*              y_tile,
                                uint8_t*              cb_tile,
                                uint8_t*              cr_tile,
                                uint16_t              mx,
                                uint16_t              my,
                                uint16_t              mcu_w_px,
                                uint16_t              mcu_h_px,
                                uint8_t*              out_buf)
{
  ra8_err_t e = ra8_jpeg_sw_priv_mcu_y(d, br, y_tile, mcu_w_px);
  if (e != k_ra8_ok) {
    return e;
  }
  if (d->ncomp == 3U) {
    e = ra8_jpeg_sw_priv_mcu_chroma(d, br, cb_tile, cr_tile);
    if (e != k_ra8_ok) {
      return e;
    }
  }
  dec_emit_mcu_rgb(d, y_tile, cb_tile, cr_tile, mx, my, mcu_w_px, mcu_h_px, out_buf);
  return k_ra8_ok;
}

/**
 * @brief Prime the bit reader and reset the DC predictors for a scan.
 *
 * @details
 * Positions the entropy bit reader at the decoder cursor and zeroes
 * every component's DC predictor, exactly as T.81 sec F.2.1.3.1
 * requires at the start of a scan.
 *
 * @param[in,out] d  Decoder context (DC predictors reset).
 * @param[out]    br Bit reader to initialise over the scan data.
 *
 * @pre `d->cursor` sits at the first entropy-coded byte (post-SOS).
 * @pre `br` is non-NULL (module-internal call chain).
 * @post `br` reads from `d->src` starting at `d->cursor`.
 * @post All `d->comp_dc_pred[]` entries are zero.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static void dec_scan_begin(ra8_jpeg_dec_ctx_t* d, ra8_jpeg_bitreader_t* br)
{
  br->buf     = d->src;
  br->len     = d->src_len;
  br->pos     = d->cursor;
  br->acc     = 0U;
  br->nbits   = 0U;
  br->had_eoi = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_jpeg_max_comps; i++) {
    d->comp_dc_pred[i] = 0;
  }
}

/**
 * @brief Decode the whole entropy-coded segment, MCU by MCU.
 *
 * @details
 * T.81 sec F.2.2 scan decode: sizes the MCU grid from the SOF0
 * sampling factors, primes the bit reader and DC predictors via
 * `dec_scan_begin()`, then walks every MCU through
 * `dec_decode_mcu()`, which reconstructs and emits its RGB pixels.
 *
 * @param[in,out] d           Decoder context (cursor lands after the scan).
 * @param[out]    out_buf     Destination RGB888 image buffer.
 * @param[in]     out_buf_len Capacity of `out_buf` in bytes.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 Whole image decoded.
 * @retval k_ra8_err_invalid_size   `out_buf` smaller than w*h*3 bytes.
 * @retval k_ra8_err_protocol_error Entropy-stream error in any MCU.
 *
 * @pre A successful SOF0 + SOS parse preceded this call.
 * @pre `d->cursor` sits at the first entropy-coded byte.
 * @post On success `d->cursor` advanced past the scan data.
 * @post On error already-emitted pixels remain valid in `out_buf`.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t dec_decode_scan(ra8_jpeg_dec_ctx_t* d, uint8_t* out_buf, uint32_t out_buf_len)
{
  uint32_t need = (uint32_t)d->width * (uint32_t)d->height * (uint32_t)k_ra8_jpeg_rgb_components;
  if (out_buf_len < need) {
    return k_ra8_err_invalid_size;
  }

  ra8_jpeg_bitreader_t br;
  dec_scan_begin(d, &br);

  /*
   * Cross-function invariant: ra8_jpeg_sw_priv_parse_sof0() only returns
   * success for the 4:4:4 and 4:2:0 chroma layouts (and grayscale), all
   * of which set hmax and vmax to a non-zero value.
   * The clang static analyzer (scan-build, core.DivideZero) cannot follow
   * the cross-function invariant, so re-state it here as an assertion to
   * make the property locally provable to the analyzer and to NASA Power-
   * of-10 Rule 5 readers.
   */
  assert(d->hmax > 0U);
  assert(d->vmax > 0U);

  uint16_t mcu_w_px = (uint16_t)((uint16_t)k_ra8_jpeg_block_dim * d->hmax);
  uint16_t mcu_h_px = (uint16_t)((uint16_t)k_ra8_jpeg_block_dim * d->vmax);
  uint16_t mcus_x   = (uint16_t)(((d->width + mcu_w_px) - 1U) / mcu_w_px);
  uint16_t mcus_y   = (uint16_t)(((d->height + mcu_h_px) - 1U) / mcu_h_px);

  uint8_t y_tile[(uint32_t)k_ra8_jpeg_mcu_max_dim * (uint32_t)k_ra8_jpeg_mcu_max_dim];
  uint8_t cb_tile[(uint32_t)k_ra8_jpeg_block_size];
  uint8_t cr_tile[(uint32_t)k_ra8_jpeg_block_size];

  for (uint16_t my = 0U; my < mcus_y; my++) {
    for (uint16_t mx = 0U; mx < mcus_x; mx++) {
      ra8_err_t e =
        dec_decode_mcu(d, &br, y_tile, cb_tile, cr_tile, mx, my, mcu_w_px, mcu_h_px, out_buf);
      if (e != k_ra8_ok) {
        return e;
      }
    }
  }
  d->cursor = br.pos;
  return k_ra8_ok;
}

/**
 * @brief Handle a non-SOF marker in the decode dispatch chain.
 *
 * @details
 * Tail of `ra8_jpeg_sw_priv_dispatch()`: routes the already-extracted
 * marker code to the DQT/DHT/SOS parsers, flags EOI, consumes RST
 * markers as standalone bytes, and skips unrecognized APPn/COM
 * segments via `ra8_jpeg_sw_priv_skip_segment()`.
 *
 * @param[in,out] d       Decoder context (cursor advances).
 * @param[in]     mk      Marker code (0xFFxx) to route.
 * @param[in]     got_sof Whether SOF0 has been parsed yet.
 * @param[out]    action  What the driver should do next.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 Marker handled (see `*action`).
 * @retval k_ra8_err_protocol_error Malformed payload or SOS before SOF.
 * @retval k_ra8_err_not_supported  Propagated from the segment parsers.
 *
 * @pre `mk` is not SOF0 and not an unsupported SOFn (caller-routed).
 * @pre `*action` was preset to k_ra8_jpeg_dec_continue.
 * @post `d->cursor` advanced past any sized payload.
 * @post `*action` is a ::ra8_jpeg_dec_marker_action_t value.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t dec_dispatch_tail(ra8_jpeg_dec_ctx_t*           d,
                                   uint16_t                      mk,
                                   bool                          got_sof,
                                   ra8_jpeg_dec_marker_action_t* action)
{
  if (mk == (uint16_t)k_ra8_jpeg_marker_dqt) {
    return ra8_jpeg_sw_priv_parse_dqt(d);
  }
  if (mk == (uint16_t)k_ra8_jpeg_marker_dht) {
    return ra8_jpeg_sw_priv_parse_dht(d);
  }
  if (mk == (uint16_t)k_ra8_jpeg_marker_sos) {
    if (!got_sof) {
      return k_ra8_err_protocol_error;
    }
    ra8_err_t e = ra8_jpeg_sw_priv_parse_sos(d);
    if (e != k_ra8_ok) {
      return e;
    }
    *action = k_ra8_jpeg_dec_scan;
    return k_ra8_ok;
  }
  if (mk == (uint16_t)k_ra8_jpeg_marker_eoi) {
    *action = k_ra8_jpeg_dec_eoi;
    return k_ra8_ok;
  }
  if (mk >= (uint16_t)k_ra8_jpeg_marker_rst0 && mk <= (uint16_t)k_ra8_jpeg_marker_rst7) {
    /* Standalone marker, no length. */
    return k_ra8_ok;
  }
  return ra8_jpeg_sw_priv_skip_segment(d);
}

/** @brief Implementation of `ra8_jpeg_sw_priv_dispatch()` -- marker extraction + routing. */
RA8_PRIV ra8_err_t ra8_jpeg_sw_priv_dispatch(ra8_jpeg_dec_ctx_t*           d,
                                             bool*                         got_sof,
                                             ra8_jpeg_dec_marker_action_t* action)
{
  *action = k_ra8_jpeg_dec_continue;
  if (d->src[d->cursor] != (uint8_t)k_ra8_jpeg_marker_byte) {
    return k_ra8_err_protocol_error;
  }
  while (d->cursor < d->src_len && d->src[d->cursor] == (uint8_t)k_ra8_jpeg_marker_byte) {
    d->cursor++;
  }
  if (d->cursor >= d->src_len) {
    return k_ra8_err_protocol_error;
  }
  uint8_t m = d->src[d->cursor];
  d->cursor++;
  uint16_t mk = (uint16_t)(k_jpeg_marker_ff00 | m);

  if (mk == (uint16_t)k_ra8_jpeg_marker_sof0) {
    ra8_err_t e = ra8_jpeg_sw_priv_parse_sof0(d);
    if (e != k_ra8_ok) {
      return e;
    }
    *got_sof = true;
    return k_ra8_ok;
    // mcdc-deactivated: ra8_jpeg_sw_priv_dispatch unsupported-SOFn detector (SOF1..SOFF excluding DHT/SOF8); identical co-dependence rationale as the SOF0 detector decision in ra8_jpeg_sw_get_dimensions -- markers >= 0xFFC1 in the JPEG spec are always <= 0xFFCF.
  }
  if (mk >= k_jpeg_marker_sof1 && mk <= k_jpeg_marker_sof_hi &&
      mk != (uint16_t)k_ra8_jpeg_marker_dht && mk != k_jpeg_marker_jpg) {
    return k_ra8_err_not_supported;
  }
  return dec_dispatch_tail(d, mk, *got_sof, action);
}

/**
 * @brief Marker-walk driver for the whole-buffer decode.
 *
 * @details
 * Loops `ra8_jpeg_sw_priv_dispatch()` over the stream until an SOS
 * hands off to `dec_decode_scan()`, an EOI ends the walk without a
 * scan (protocol error), or the stream is exhausted.
 *
 * @param[in,out] d           Initialised decoder context (cursor at 2).
 * @param[out]    out_buf     Destination RGB888 buffer.
 * @param[in]     out_buf_len Capacity of `out_buf` in bytes.
 * @param[out]    out_w       Receives the image width on scan start.
 * @param[out]    out_h       Receives the image height on scan start.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 Image decoded into `out_buf`.
 * @retval k_ra8_err_invalid_size   `out_buf` too small for the frame.
 * @retval k_ra8_err_protocol_error Malformed stream or no scan found.
 * @retval k_ra8_err_not_supported  Non-baseline stream feature.
 *
 * @pre `d->src`/`d->src_len` describe an SOI-verified stream.
 * @pre All output pointers are non-NULL (public API validated them).
 * @post On success `*out_w`/`*out_h` hold the SOF0 dimensions.
 * @post On error the output buffer contents are unspecified.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t dec_run(ra8_jpeg_dec_ctx_t* d,
                         uint8_t*            out_buf,
                         uint32_t            out_buf_len,
                         uint16_t*           out_w,
                         uint16_t*           out_h)
{
  bool got_sof = false;
  while (d->cursor < d->src_len) {
    ra8_jpeg_dec_marker_action_t action = k_ra8_jpeg_dec_continue;
    ra8_err_t                    e      = ra8_jpeg_sw_priv_dispatch(d, &got_sof, &action);
    if (e != k_ra8_ok) {
      return e;
    }
    if (action == k_ra8_jpeg_dec_scan) {
      *out_w = d->width;
      *out_h = d->height;
      return dec_decode_scan(d, out_buf, out_buf_len);
    }
    if (action == k_ra8_jpeg_dec_eoi) {
      break;
    }
  }
  return k_ra8_err_protocol_error;
}

ra8_err_t ra8_jpeg_sw_decode(const uint8_t* jpeg_buf,
                             uint32_t       jpeg_len,
                             uint8_t*       out_buf,
                             uint32_t       out_buf_len,
                             uint16_t*      out_w,
                             uint16_t*      out_h)
{
  RA8_CHECK_NULL_PTR(jpeg_buf, s_tag, "jpeg_buf is NULL");
  RA8_CHECK_NULL_PTR(out_buf, s_tag, "out_buf is NULL");
  RA8_CHECK_NULL_PTR(out_w, s_tag, "out_w is NULL");
  RA8_CHECK_NULL_PTR(out_h, s_tag, "out_h is NULL");
  if (jpeg_len < 4U) {
    return k_ra8_err_invalid_size;
  }

  /* Decoder context is large (4 Huffman tables + 2 quant tables);
   * allocate as `static` so it doesn't blow the stack budget the
   * project enforces via `-Wstack-usage`. The codec is documented
   * as not thread-safe, so the static is fine. */
  static ra8_jpeg_dec_ctx_t s_d;
  ra8_jpeg_dec_ctx_t*       d = &s_d;
  memset(d, 0, sizeof(*d));
  d->src     = jpeg_buf;
  d->src_len = jpeg_len;

  if (read_be16(jpeg_buf) != (uint16_t)k_ra8_jpeg_marker_soi) {
    return k_ra8_err_protocol_error;
  }
  d->cursor = 2U;

  return dec_run(d, out_buf, out_buf_len, out_w, out_h);
}
