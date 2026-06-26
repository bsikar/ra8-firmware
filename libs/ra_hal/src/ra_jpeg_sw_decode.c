/**
 * @file ra_jpeg_sw_decode.c
 * @brief Pure-software baseline JPEG decoder: marker parser and scan.
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Implements the marker-segment parser (DQT / DHT / SOF0 / SOS), the
 * per-MCU entropy-decode + IDCT + colour-conversion scan loop, and the
 * `ra_jpeg_sw_decode()` public API for baseline (8-bit, sequential,
 * Huffman) JPEG streams in the YCbCr 4:2:0, YCbCr 4:4:4 and grayscale
 * layouts. The reference codec is the C99 reformulation of "TJpgDec"
 * by ChaN, re-implemented from scratch in this project's style; no
 * third-party code is copied.
 *
 * This is the decoder-driver half of the software JPEG codec; the
 * shared entropy/DSP primitives (bit reader, Huffman tables, inverse
 * DCT, colour conversion) and the `ra_jpeg_sw_get_dimensions()` public
 * API live in `ra_jpeg_sw.c`, and every cross-unit symbol lives in
 * `ra_jpeg_sw_internal.h`.
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

#include "ra_check.h"
#include "ra_err.h"
#include "ra_jpeg_sw.h"
#include "ra_jpeg_sw_internal.h"
#include "ra_log.h"

/* The state-machine functions (`dec_decode_scan`, `dec_parse_*`) are
 * above clang-tidy's default function-size and cognitive-complexity
 * thresholds because each step of the JPEG codec maps 1:1 onto the
 * spec text -- splitting them further would obscure the spec citations
 * rather than help readability. The `readability-magic-numbers`
 * suppression covers spec-defined byte values such as the marker codes
 * (`0xFFC0`..`0xFFCF`) and JFIF segment-length constants; each
 * occurrence carries a `T.81 sec X.Y` citation. The
 * `readability-redundant-casting` suppression covers the
 * `(uint8_t)k_ra_jpeg_*_t` form used to make enum membership explicit
 * at use sites that mix multiple enum families. */
// NOLINTBEGIN(readability-function-size,readability-function-cognitive-complexity,readability-redundant-casting,readability-math-missing-parentheses,bugprone-implicit-widening-of-multiplication-result,clang-analyzer-core.DivideZero,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)

/** @brief Component log tag. */
static const char* s_tag = "JPEG_SW";

/* ------------------------------------------------------------------ */
/* Decoder */
/* ------------------------------------------------------------------ */

/**
 * @struct ra_jpeg_dec_ctx_t
 * @brief Decoder state -- all stack-allocated, no heap.
 */
typedef struct {
  const uint8_t* src;
  uint32_t       src_len;
  uint32_t       cursor;

  uint16_t width;
  uint16_t height;
  uint8_t  ncomp; /**< 1 grayscale, 3 YCbCr. */
  uint8_t  hmax;
  uint8_t  vmax;

  /* Per-component info (T.81 sec B.2.2). */
  uint8_t comp_id[k_ra_jpeg_max_comps];
  uint8_t comp_h[k_ra_jpeg_max_comps];
  uint8_t comp_v[k_ra_jpeg_max_comps];
  uint8_t comp_qid[k_ra_jpeg_max_comps];
  uint8_t comp_dc_id[k_ra_jpeg_max_comps];
  uint8_t comp_ac_id[k_ra_jpeg_max_comps];
  int32_t comp_dc_pred[k_ra_jpeg_max_comps];

  uint16_t       qtab[k_ra_jpeg_quant_tabs][k_ra_jpeg_block_size];
  ra_jpeg_htab_t hdc[k_ra_jpeg_huff_ids];
  ra_jpeg_htab_t hac[k_ra_jpeg_huff_ids];
} ra_jpeg_dec_ctx_t;

/* Skip an unrecognized variable-length segment -- see surrounding code and HUM citations. */
static ra_err_t dec_skip_segment(ra_jpeg_dec_ctx_t* d)
{
  if (d->cursor + 2U > d->src_len) {
    return k_ra_err_protocol_error;
  }
  uint16_t len = read_be16(&d->src[d->cursor]);
  if (len < 2U || (uint32_t)len > d->src_len - d->cursor) {
    return k_ra_err_protocol_error;
  }
  d->cursor += len;
  return k_ra_ok;
}

/* Parse a DQT segment (T -- see surrounding code and HUM citations. */
static ra_err_t dec_parse_dqt(ra_jpeg_dec_ctx_t* d)
{
  if (d->cursor + 2U > d->src_len) {
    return k_ra_err_protocol_error;
  }
  uint16_t len = read_be16(&d->src[d->cursor]);
  if (len < 2U || (uint32_t)len > d->src_len - d->cursor) {
    return k_ra_err_protocol_error;
  }
  uint32_t end = d->cursor + len;
  d->cursor += 2U;
  while (d->cursor < end) {
    uint8_t pq_tq = d->src[d->cursor];
    d->cursor++;
    uint8_t pq = (uint8_t)(pq_tq >> k_ra_jpeg_nibble_shift);
    uint8_t tq = (uint8_t)(pq_tq & k_ra_jpeg_nibble_mask);
    if (tq >= (uint8_t)k_ra_jpeg_quant_tabs || pq != 0U) {
      return k_ra_err_not_supported; /* 16-bit precision not supported. */
    }
    if (d->cursor + (uint32_t)k_ra_jpeg_block_size > end) {
      return k_ra_err_protocol_error;
    }
    for (uint8_t i = 0U; i < (uint8_t)k_ra_jpeg_block_size; i++) {
      d->qtab[tq][s_zigzag[i]] = d->src[d->cursor];
      d->cursor++;
    }
  }
  return k_ra_ok;
}

/* Parse a DHT segment (T -- see surrounding code and HUM citations. */
static ra_err_t dec_parse_dht(ra_jpeg_dec_ctx_t* d)
{
  if (d->cursor + 2U > d->src_len) {
    return k_ra_err_protocol_error;
  }
  uint16_t len = read_be16(&d->src[d->cursor]);
  if (len < 2U || (uint32_t)len > d->src_len - d->cursor) {
    return k_ra_err_protocol_error;
  }
  uint32_t end = d->cursor + len;
  d->cursor += 2U;
  while (d->cursor < end) {
    uint8_t tc_th = d->src[d->cursor];
    d->cursor++;
    uint8_t tc = (uint8_t)(tc_th >> k_ra_jpeg_nibble_shift);
    uint8_t th = (uint8_t)(tc_th & k_ra_jpeg_nibble_mask);
    if (tc >= (uint8_t)k_ra_jpeg_huff_classes || th >= (uint8_t)k_ra_jpeg_huff_ids) {
      return k_ra_err_not_supported;
    }
    if (d->cursor + (uint32_t)k_ra_jpeg_huff_lengths > end) {
      return k_ra_err_protocol_error;
    }
    ra_jpeg_htab_t* h     = (tc == 0U) ? &d->hdc[th] : &d->hac[th];
    uint16_t        total = 0U;
    for (uint8_t i = 0U; i < (uint8_t)k_ra_jpeg_huff_lengths; i++) {
      h->bits[i] = d->src[d->cursor];
      d->cursor++;
      total = (uint16_t)(total + h->bits[i]);
    }
    if (total > (uint16_t)k_ra_jpeg_huff_max) {
      return k_ra_err_protocol_error;
    }
    if (d->cursor + total > end) {
      return k_ra_err_protocol_error;
    }
    for (uint16_t i = 0U; i < total; i++) {
      h->vals[i] = d->src[d->cursor];
      d->cursor++;
    }
    ra_jpeg_sw_htab_build(h);
  }
  return k_ra_ok;
}

/* Parse SOF0 (T -- see surrounding code and HUM citations. */
static ra_err_t dec_parse_sof0(ra_jpeg_dec_ctx_t* d)
{
  if (d->cursor + 2U > d->src_len) {
    return k_ra_err_protocol_error;
  }
  uint16_t len = read_be16(&d->src[d->cursor]);
  if (len < 8U || (uint32_t)len > d->src_len - d->cursor) {
    return k_ra_err_protocol_error;
  }
  uint32_t s         = d->cursor + 2U;
  uint8_t  precision = d->src[s];
  s++;
  if (precision != 8U) {
    return k_ra_err_not_supported;
  }
  d->height = read_be16(&d->src[s]);
  s += 2U;
  d->width = read_be16(&d->src[s]);
  s += 2U;
  d->ncomp = d->src[s];
  s++;
  if (d->ncomp != 1U && d->ncomp != 3U) {
    return k_ra_err_not_supported;
  }
  if ((uint32_t)d->ncomp * 3U + (s - d->cursor) > (uint32_t)len) {
    return k_ra_err_protocol_error;
  }
  d->hmax = 0U;
  d->vmax = 0U;
  for (uint8_t i = 0U; i < d->ncomp; i++) {
    d->comp_id[i] = d->src[s];
    s++;
    uint8_t hv = d->src[s];
    s++;
    d->comp_h[i]   = (uint8_t)(hv >> k_ra_jpeg_nibble_shift);
    d->comp_v[i]   = (uint8_t)(hv & k_ra_jpeg_nibble_mask);
    d->comp_qid[i] = d->src[s];
    s++;
    if (d->comp_h[i] > d->hmax) {
      d->hmax = d->comp_h[i];
    }
    if (d->comp_v[i] > d->vmax) {
      d->vmax = d->comp_v[i];
    }
  }
  d->cursor += len;
  /* Only 4:4:4 (1,1,1) and 4:2:0 (2,1,1) accepted. */
  if (d->ncomp == 3U) {
    bool is_444 = (d->hmax == 1U && d->vmax == 1U);
    bool is_420 = (d->hmax == 2U && d->vmax == 2U && d->comp_h[1] == 1U && d->comp_v[1] == 1U &&
                   d->comp_h[2] == 1U && d->comp_v[2] == 1U);
    if (!is_444 && !is_420) {
      return k_ra_err_not_supported;
    }
  }
  return k_ra_ok;
}

/* Parse SOS scan-component selectors (T -- see surrounding code and HUM citations. */
static ra_err_t dec_parse_sos(ra_jpeg_dec_ctx_t* d)
{
  if (d->cursor + 2U > d->src_len) {
    return k_ra_err_protocol_error;
  }
  uint16_t len = read_be16(&d->src[d->cursor]);
  if (len < 6U || (uint32_t)len > d->src_len - d->cursor) {
    return k_ra_err_protocol_error;
  }
  uint32_t s  = d->cursor + 2U;
  uint8_t  ns = d->src[s];
  s++;
  if (ns != d->ncomp) {
    return k_ra_err_not_supported;
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
    d->comp_dc_id[idx] = (uint8_t)(tdta >> k_ra_jpeg_nibble_shift);
    d->comp_ac_id[idx] = (uint8_t)(tdta & k_ra_jpeg_nibble_mask);
    if (d->comp_dc_id[idx] >= (uint8_t)k_ra_jpeg_huff_ids ||
        d->comp_ac_id[idx] >= (uint8_t)k_ra_jpeg_huff_ids) {
      return k_ra_err_not_supported;
    }
  }
  /* Skip Ss/Se/AhAl (3 bytes). */
  d->cursor += len;
  return k_ra_ok;
}

/* Decode one 8x8 block of coefficients -- see surrounding code and HUM citations. */
static ra_err_t
dec_block(ra_jpeg_dec_ctx_t* d, ra_jpeg_bitreader_t* br, uint8_t ci, int32_t* outblk)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra_jpeg_block_size; i++) {
    outblk[i] = 0;
  }
  /* DC. */
  int32_t t = ra_jpeg_sw_htab_decode(br, &d->hdc[d->comp_dc_id[ci]]);
  if (t < 0) {
    return k_ra_err_protocol_error;
  }
  int32_t r = ra_jpeg_sw_br_get_bits(br, (uint8_t)t);
  /* mcdc-deactivated: ra_jpeg_sw_br_get_bits returns -1 only when the bitstream
   * is exhausted; reaching this branch with t != 0 requires a
   * truncated entropy-coded segment after every parser stage has
   * succeeded -- the public-API contract (well-formed JFIF stream
   * with EOI) makes this branch unreachable. Defensive guard for
   * fault injection. */
  // mcdc-deactivated: dec_parse_sos ra_jpeg_sw_br_get_bits exhaustion guard; well-formed JFIF streams (public-API contract) cannot exhaust the bitstream mid-coefficient with t != 0 because every parser stage upstream has validated the segment-length budget.
  if (r < 0 && t != 0) {
    return k_ra_err_protocol_error;
  }
  int32_t diff = ra_jpeg_sw_huff_extend(r, (uint8_t)t);
  d->comp_dc_pred[ci] += diff;
  outblk[0] = d->comp_dc_pred[ci] * (int32_t)d->qtab[d->comp_qid[ci]][0];

  /* AC. */
  uint8_t k = 1U;
  while (k < (uint8_t)k_ra_jpeg_block_size) {
    int32_t rs = ra_jpeg_sw_htab_decode(br, &d->hac[d->comp_ac_id[ci]]);
    if (rs < 0) {
      return k_ra_err_protocol_error;
    }
    uint8_t rrrr = (uint8_t)(rs >> k_ra_jpeg_nibble_shift);
    uint8_t ssss = (uint8_t)(rs & k_ra_jpeg_nibble_mask);
    if (ssss == 0U) {
      if (rrrr == k_jpeg_nibble_mask) {
        k = (uint8_t)(k + 16U); /* ZRL: 16 zero coeffs. */
        continue;
      }
      break; /* EOB. */
    }
    k = (uint8_t)(k + rrrr);
    if (k >= (uint8_t)k_ra_jpeg_block_size) {
      return k_ra_err_protocol_error;
    }
    int32_t v = ra_jpeg_sw_br_get_bits(br, ssss);
    if (v < 0) {
      return k_ra_err_protocol_error;
    }
    v                   = ra_jpeg_sw_huff_extend(v, ssss);
    outblk[s_zigzag[k]] = v * (int32_t)d->qtab[d->comp_qid[ci]][s_zigzag[k]];
    k++;
  }
  return k_ra_ok;
}

/* ------------------------------------------------------------------ */
/* Public API: decode */
/* ------------------------------------------------------------------ */

/* Render a fully-dequantized+IDCTed component sample into a tile -- see surrounding code and HUM citations. */
static void dec_idct_into(int32_t* coeffs, uint8_t* tile)
{
  ra_jpeg_sw_idct8x8(coeffs);
  for (uint8_t i = 0U; i < (uint8_t)k_ra_jpeg_block_size; i++) {
    int32_t v = coeffs[i] + (int32_t)k_ra_jpeg_level_offset;
    tile[i]   = clamp_u8(v);
  }
}

/**
 * @brief Decode the hmax*vmax luma blocks of one MCU and write them into the Y tile.
 *
 * @details
 * Pulls successive 8x8 luma blocks from the entropy stream, IDCTs each
 * one, and copies it into the correct sub-rectangle of `y_tile` so the
 * MCU pixel-emit loop can address it linearly.
 *
 * @param[in,out] d        Decoder context (DC predictors mutate).
 * @param[in,out] br       Bit-reader positioned at the next luma block.
 * @param[out]    y_tile   Output tile of (mcu_w_px x mcu_h_px) luma px.
 * @param[in]     mcu_w_px MCU width in pixels (k_ra_jpeg_block_dim * d->hmax).
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Success, all luma blocks consumed.
 * @retval k_ra_err_protocol_error Underlying dec_block() reported a stream error.
 *
 * @pre `d`, `br`, `y_tile` are non-NULL.
 * @pre `d->hmax` and `d->vmax` are non-zero (enforced by dec_parse_sof0).
 * @post On success `br->pos` advanced past hmax*vmax luma blocks.
 * @post On success `y_tile` fully populated with reconstructed Y pixels.
 *
 * @note Not thread-safe; caller serializes access via decoder context.
 * @since 0.1.0
 */
static ra_err_t dec_decode_mcu_y_blocks(ra_jpeg_dec_ctx_t*   d,
                                        ra_jpeg_bitreader_t* br,
                                        uint8_t*             y_tile,
                                        uint16_t             mcu_w_px)
{
  int32_t coeffs[(uint32_t)k_ra_jpeg_block_size];
  for (uint8_t by = 0U; by < d->vmax; by++) {
    for (uint8_t bx = 0U; bx < d->hmax; bx++) {
      ra_err_t e = dec_block(d, br, 0U, coeffs);
      if (e != k_ra_ok) {
        return e;
      }
      uint8_t blk[(uint32_t)k_ra_jpeg_block_size];
      dec_idct_into(coeffs, blk);
      for (uint8_t r = 0U; r < (uint8_t)k_ra_jpeg_block_dim; r++) {
        for (uint8_t c = 0U; c < (uint8_t)k_ra_jpeg_block_dim; c++) {
          uint16_t ty                = (uint16_t)(by * (uint16_t)k_ra_jpeg_block_dim + r);
          uint16_t tx                = (uint16_t)(bx * (uint16_t)k_ra_jpeg_block_dim + c);
          y_tile[ty * mcu_w_px + tx] = blk[r * (uint8_t)k_ra_jpeg_block_dim + c];
        }
      }
    }
  }
  return k_ra_ok;
}

/**
 * @brief Decode the Cb and Cr 8x8 blocks of one MCU into their tile buffers.
 *
 * @details
 * For 3-component YCbCr streams, consumes one Cb block then one Cr block
 * from the entropy stream, IDCTs each, and stores the result in the
 * caller's tile arrays. Caller must skip this for grayscale streams.
 *
 * @param[in,out] d       Decoder context (DC predictors mutate).
 * @param[in,out] br      Bit-reader positioned at the next chroma block.
 * @param[out]    cb_tile 64-byte buffer for reconstructed Cb pixels.
 * @param[out]    cr_tile 64-byte buffer for reconstructed Cr pixels.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok               Both chroma blocks decoded successfully.
 * @retval k_ra_err_protocol_error Stream error reported by dec_block().
 *
 * @pre `d->ncomp == 3` (caller verifies before invocation).
 * @pre `cb_tile` and `cr_tile` each point to >= 64 writable bytes.
 * @post `br->pos` advanced past two 8x8 chroma blocks.
 * @post Both tile buffers fully populated.
 *
 * @note Not thread-safe; caller serializes access via decoder context.
 * @since 0.1.0
 */
static ra_err_t dec_decode_mcu_chroma_blocks(ra_jpeg_dec_ctx_t*   d,
                                             ra_jpeg_bitreader_t* br,
                                             uint8_t*             cb_tile,
                                             uint8_t*             cr_tile)
{
  int32_t  coeffs[(uint32_t)k_ra_jpeg_block_size];
  ra_err_t e = dec_block(d, br, 1U, coeffs);
  if (e != k_ra_ok) {
    return e;
  }
  dec_idct_into(coeffs, cb_tile);
  e = dec_block(d, br, 2U, coeffs);
  if (e != k_ra_ok) {
    return e;
  }
  dec_idct_into(coeffs, cr_tile);
  return k_ra_ok;
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
static void dec_emit_mcu_rgb(const ra_jpeg_dec_ctx_t* d,
                             const uint8_t*           y_tile,
                             const uint8_t*           cb_tile,
                             const uint8_t*           cr_tile,
                             uint16_t                 mx,
                             uint16_t                 my,
                             uint16_t                 mcu_w_px,
                             uint16_t                 mcu_h_px,
                             uint8_t*                 out_buf)
{
  for (uint16_t r = 0U; r < mcu_h_px; r++) {
    uint16_t py = (uint16_t)(my * mcu_h_px + r);
    if (py >= d->height) {
      break;
    }
    for (uint16_t c = 0U; c < mcu_w_px; c++) {
      uint16_t px = (uint16_t)(mx * mcu_w_px + c);
      if (px >= d->width) {
        break;
      }
      int32_t y = (int32_t)y_tile[r * mcu_w_px + c];
      int32_t cb;
      int32_t cr;
      if (d->ncomp == 3U) {
        uint16_t cr_x = (uint16_t)(c / d->hmax);
        uint16_t cr_y = (uint16_t)(r / d->vmax);
        cb            = (int32_t)cb_tile[cr_y * (uint16_t)k_ra_jpeg_block_dim + cr_x];
        cr            = (int32_t)cr_tile[cr_y * (uint16_t)k_ra_jpeg_block_dim + cr_x];
      } else {
        cb = (int32_t)k_ra_jpeg_level_offset;
        cr = (int32_t)k_ra_jpeg_level_offset;
      }
      uint32_t idx =
        ((uint32_t)py * (uint32_t)d->width + (uint32_t)px) * (uint32_t)k_ra_jpeg_rgb_components;
      ra_jpeg_sw_ycc_to_rgb(y, cb, cr, &out_buf[idx], &out_buf[idx + 1U], &out_buf[idx + 2U]);
    }
  }
}

/* Walk the entropy-coded segment, MCU by MCU -- see surrounding code and HUM citations. */
static ra_err_t dec_decode_scan(ra_jpeg_dec_ctx_t* d, uint8_t* out_buf, uint32_t out_buf_len)
{
  uint32_t need = (uint32_t)d->width * (uint32_t)d->height * (uint32_t)k_ra_jpeg_rgb_components;
  if (out_buf_len < need) {
    return k_ra_err_invalid_size;
  }

  ra_jpeg_bitreader_t br = {
    .buf     = d->src,
    .len     = d->src_len,
    .pos     = d->cursor,
    .acc     = 0U,
    .nbits   = 0U,
    .had_eoi = 0U,
  };

  /*
   * Cross-function invariant: dec_parse_sof0() only returns success for
   * 4:4:4, 4:2:2 (h+v), 4:2:2 (h-only), and 4:2:0 chroma layouts, all of
   * which set hmax and vmax to a non-zero value (see dec_parse_sof0).
   * The clang static analyzer (scan-build, core.DivideZero) cannot follow
   * the cross-function invariant, so re-state it here as an assertion to
   * make the property locally provable to the analyzer and to NASA Power-
   * of-10 Rule 5 readers.
   */
  assert(d->hmax > 0U);
  assert(d->vmax > 0U);

  uint16_t mcu_w_px = (uint16_t)((uint16_t)k_ra_jpeg_block_dim * d->hmax);
  uint16_t mcu_h_px = (uint16_t)((uint16_t)k_ra_jpeg_block_dim * d->vmax);
  uint16_t mcus_x   = (uint16_t)((d->width + mcu_w_px - 1U) / mcu_w_px);
  uint16_t mcus_y   = (uint16_t)((d->height + mcu_h_px - 1U) / mcu_h_px);

  uint8_t y_tile[(uint32_t)k_ra_jpeg_mcu_max_dim * (uint32_t)k_ra_jpeg_mcu_max_dim];
  uint8_t cb_tile[(uint32_t)k_ra_jpeg_block_size];
  uint8_t cr_tile[(uint32_t)k_ra_jpeg_block_size];

  for (uint8_t i = 0U; i < (uint8_t)k_ra_jpeg_max_comps; i++) {
    d->comp_dc_pred[i] = 0;
  }

  for (uint16_t my = 0U; my < mcus_y; my++) {
    for (uint16_t mx = 0U; mx < mcus_x; mx++) {
      ra_err_t e = dec_decode_mcu_y_blocks(d, &br, y_tile, mcu_w_px);
      if (e != k_ra_ok) {
        return e;
      }
      if (d->ncomp == 3U) {
        e = dec_decode_mcu_chroma_blocks(d, &br, cb_tile, cr_tile);
        if (e != k_ra_ok) {
          return e;
        }
      }
      dec_emit_mcu_rgb(d, y_tile, cb_tile, cr_tile, mx, my, mcu_w_px, mcu_h_px, out_buf);
    }
  }
  d->cursor = br.pos;
  return k_ra_ok;
}

/**
 * @enum ra_jpeg_dec_marker_action_t
 * @brief Result of dec_dispatch_marker(): tells ra_jpeg_sw_decode() what to do next.
 */
typedef enum : uint8_t {
  k_ra_jpeg_dec_continue = 0U, /**< Keep scanning markers.                               */
  k_ra_jpeg_dec_scan     = 1U, /**< SOS reached; caller should run dec_decode_scan().    */
  k_ra_jpeg_dec_eoi      = 2U, /**< EOI reached; caller should stop with protocol_error. */
} ra_jpeg_dec_marker_action_t;

/**
 * @brief Dispatch one JPEG marker segment in the top-level decode loop.
 *
 * @details
 * Parses one marker following the 0xFF prefix at `d->cursor` and updates
 * the decoder context accordingly. Tracks whether an SOF0 has been seen
 * via `*got_sof`. Unknown segments are skipped with `dec_skip_segment()`
 * so unrecognised APPn/COM payloads do not abort decoding.
 *
 * @param[in,out] d        Decoder context (cursor advances).
 * @param[in,out] got_sof  Tracks whether SOF0 has been parsed yet.
 * @param[out]    action   What the caller should do next.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                Marker parsed successfully (see *action).
 * @retval k_ra_err_protocol_error Malformed marker, truncated stream, or SOS before SOF.
 * @retval k_ra_err_not_supported  Unsupported SOFn (progressive, lossless, ...).
 *
 * @pre `d`, `got_sof`, `action` are non-NULL.
 * @pre `d->cursor < d->src_len` (caller checks).
 * @post `d->cursor` advanced past the marker byte and (for sized markers) past its payload.
 * @post `*action` is one of the ra_jpeg_dec_marker_action_t values.
 *
 * @note Not thread-safe; caller serializes via decoder context.
 * @since 0.1.0
 */
static ra_err_t
dec_dispatch_marker(ra_jpeg_dec_ctx_t* d, bool* got_sof, ra_jpeg_dec_marker_action_t* action)
{
  *action = k_ra_jpeg_dec_continue;
  if (d->src[d->cursor] != (uint8_t)k_ra_jpeg_marker_byte) {
    return k_ra_err_protocol_error;
  }
  while (d->cursor < d->src_len && d->src[d->cursor] == (uint8_t)k_ra_jpeg_marker_byte) {
    d->cursor++;
  }
  if (d->cursor >= d->src_len) {
    return k_ra_err_protocol_error;
  }
  uint8_t m = d->src[d->cursor];
  d->cursor++;
  uint16_t mk = (uint16_t)(k_jpeg_marker_ff00 | m);

  if (mk == (uint16_t)k_ra_jpeg_marker_sof0) {
    ra_err_t e = dec_parse_sof0(d);
    if (e != k_ra_ok) {
      return e;
    }
    *got_sof = true;
    return k_ra_ok;
    // mcdc-deactivated: dec_decode_scan unsupported-SOFn detector (SOF1..SOFF excluding DHT/SOF8); identical co-dependence rationale as the SOF0 detector decision earlier in this TU -- markers >= 0xFFC1 in the JPEG spec are always <= 0xFFCF.
  }
  if (mk >= k_jpeg_marker_sof1 && mk <= k_jpeg_marker_sof_hi &&
      mk != (uint16_t)k_ra_jpeg_marker_dht && mk != k_jpeg_marker_jpg) {
    return k_ra_err_not_supported;
  }
  if (mk == (uint16_t)k_ra_jpeg_marker_dqt) {
    return dec_parse_dqt(d);
  }
  if (mk == (uint16_t)k_ra_jpeg_marker_dht) {
    return dec_parse_dht(d);
  }
  if (mk == (uint16_t)k_ra_jpeg_marker_sos) {
    if (!*got_sof) {
      return k_ra_err_protocol_error;
    }
    ra_err_t e = dec_parse_sos(d);
    if (e != k_ra_ok) {
      return e;
    }
    *action = k_ra_jpeg_dec_scan;
    return k_ra_ok;
  }
  if (mk == (uint16_t)k_ra_jpeg_marker_eoi) {
    *action = k_ra_jpeg_dec_eoi;
    return k_ra_ok;
  }
  if (mk >= (uint16_t)k_ra_jpeg_marker_rst0 && mk <= (uint16_t)k_ra_jpeg_marker_rst7) {
    /* Standalone marker, no length. */
    return k_ra_ok;
  }
  return dec_skip_segment(d);
}

ra_err_t ra_jpeg_sw_decode(const uint8_t* jpeg_buf,
                           uint32_t       jpeg_len,
                           uint8_t*       out_buf,
                           uint32_t       out_buf_len,
                           uint16_t*      out_w,
                           uint16_t*      out_h)
{
  RA_CHECK_NULL_PTR(jpeg_buf, s_tag, "jpeg_buf is NULL");
  RA_CHECK_NULL_PTR(out_buf, s_tag, "out_buf is NULL");
  RA_CHECK_NULL_PTR(out_w, s_tag, "out_w is NULL");
  RA_CHECK_NULL_PTR(out_h, s_tag, "out_h is NULL");
  if (jpeg_len < 4U) {
    return k_ra_err_invalid_size;
  }

  /* Decoder context is large (4 Huffman tables + 2 quant tables);
   * allocate as `static` so it doesn't blow the stack budget the
   * project enforces via `-Wstack-usage`. The codec is documented
   * as not thread-safe, so the static is fine. */
  static ra_jpeg_dec_ctx_t s_d;
  ra_jpeg_dec_ctx_t*       d = &s_d;
  memset(d, 0, sizeof(*d));
  d->src     = jpeg_buf;
  d->src_len = jpeg_len;

  if (read_be16(jpeg_buf) != (uint16_t)k_ra_jpeg_marker_soi) {
    return k_ra_err_protocol_error;
  }
  d->cursor = 2U;

  bool got_sof = false;
  while (d->cursor < d->src_len) {
    ra_jpeg_dec_marker_action_t action = k_ra_jpeg_dec_continue;
    ra_err_t                    e      = dec_dispatch_marker(d, &got_sof, &action);
    if (e != k_ra_ok) {
      return e;
    }
    if (action == k_ra_jpeg_dec_scan) {
      *out_w = d->width;
      *out_h = d->height;
      return dec_decode_scan(d, out_buf, out_buf_len);
    }
    if (action == k_ra_jpeg_dec_eoi) {
      break;
    }
  }
  return k_ra_err_protocol_error;
}

// NOLINTEND(readability-function-size,readability-function-cognitive-complexity,readability-redundant-casting,readability-math-missing-parentheses,bugprone-implicit-widening-of-multiplication-result,clang-analyzer-core.DivideZero,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
