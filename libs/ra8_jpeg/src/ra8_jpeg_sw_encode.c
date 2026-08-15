/**
 * @file ra8_jpeg_sw_encode.c
 * @brief Pure-software baseline JPEG encoder: pixel pipeline and
 *        public API.
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * Implements `ra8_jpeg_sw_encode()` for baseline (8-bit, sequential,
 * Huffman) JPEG output in the YCbCr 4:2:0 layout. The reference
 * codec is the `write_JPEG_file()` example shipped with the IJG
 * libjpeg sources and the matching pseudo-code in ITU-T T.81 Annex
 * K; it has been re-implemented here from scratch in this project's
 * style and naming conventions, with no third-party code copied.
 *
 * This unit owns the pixel pipeline: RGB->YCbCr strip conversion,
 * luma/chroma block sampling, the forward DCT, quantization and the
 * per-block Huffman emission driver. The byte/bit emitters, the
 * Annex K.3.3 reference tables and the JFIF header writers live in
 * `ra8_jpeg_sw_encode_emit.c`; the symbols shared between the two
 * units are declared in `ra8_jpeg_sw_encode_internal.h`. The decoder
 * half and the shared entropy/DSP primitives live in
 * `ra8_jpeg_sw.c`, with cross-unit symbols in
 * `ra8_jpeg_sw_internal.h`.
 *
 * Spec citations are tagged `T.81 sec X.Y "..."` and refer to
 * ITU-T Recommendation T.81 (1992) | ISO/IEC 10918-1.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_jpeg_sw.h"
#include "ra8_jpeg_sw_encode_internal.h"
#include "ra8_jpeg_sw_internal.h"
#include "ra8_log.h"

/** @brief Component log tag. */
static const char* s_tag = "JPEG_SW";

/* ------------------------------------------------------------------ */
/* Encoder quantization reference tables (T.81 Annex K.1) */
/* ------------------------------------------------------------------ */

/** @brief T.81 Annex K.1 luma quantization table. */
static const uint8_t s_quant_luma[64] = {
  16, 11,  10,  16, 24, 40, 51, 61, 12,  12,  14,  19,  26, 58, 60, 55,  14,  13,  16,  24, 40, 57,
  69, 56,  14,  17, 22, 29, 51, 87, 80,  62,  18,  22,  37, 56, 68, 109, 103, 77,  24,  35, 55, 64,
  81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98,  112, 100, 103, 99,
};

/** @brief T.81 Annex K.1 chroma quantization table. */
static const uint8_t s_quant_chroma[64] = {
  17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99,
  99, 99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
  99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
};

/* ------------------------------------------------------------------ */
/* Forward DCT (encoder) */
/* ------------------------------------------------------------------ */

/**
 * @brief 1-D forward DCT pass with JPEG normalization folded in.
 *
 * @details
 * Computes `Y[k] = sqrt(2/N)*C(k) * sum_n y[n]*cos((2n+1)*k*pi/16)`
 * over the shared Q14 cosine basis, so the encoder and decoder use a
 * numerically identical transform pair.
 *
 * @param[in]  in  8-entry spatial sample row/column.
 * @param[out] out 8-entry frequency coefficient row/column.
 *
 * @pre `in` and `out` hold 8 valid/writable int32 slots each.
 * @pre Input samples are level-shifted to be centred on zero.
 * @post `out` holds the normalized 1-D DCT of `in`.
 * @post No global state is touched.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static void fwd_dct_1d_norm(const int32_t* in, int32_t* out)
{
  for (uint8_t k = 0U; k < (uint8_t)k_ra8_jpeg_block_dim; k++) {
    int64_t s = 0;
    for (uint8_t n = 0U; n < (uint8_t)k_ra8_jpeg_block_dim; n++) {
      s += (int64_t)in[n] * (int64_t)s_dct_cos_q14[k][n];
    }
    /* s is Q14*Q0 = Q14; multiply by Q14 weight -> Q28; round to Q0. */
    int64_t r =
      ((s * (int64_t)s_dct_w_q14[k]) + (1LL << k_jpeg_idct_p1_bias_sh)) >> k_jpeg_idct_p1_sh;
    out[k] = (int32_t)r;
  }
}

/**
 * @brief 2-D forward DCT of one 8x8 block, in place.
 *
 * @details Separable two-pass transform: `fwd_dct_1d_norm()` over the
 *          rows, then over the columns of the intermediate result.
 *
 * @param[in,out] block 64-entry row-major sample block, DCTed in place.
 *
 * @pre `block` holds 64 level-shifted spatial samples.
 * @pre `block` is non-NULL (module-internal call chain).
 * @post `block` holds the 2-D DCT coefficients.
 * @post No global state is touched.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static void fdct8x8(int32_t* block)
{
  int32_t tmp[(uint32_t)k_ra8_jpeg_block_size];
  int32_t row[(uint32_t)k_ra8_jpeg_block_dim];
  int32_t col[(uint32_t)k_ra8_jpeg_block_dim];
  int32_t out[(uint32_t)k_ra8_jpeg_block_dim];
  for (uint8_t r = 0U; r < (uint8_t)k_ra8_jpeg_block_dim; r++) {
    for (uint8_t c = 0U; c < (uint8_t)k_ra8_jpeg_block_dim; c++) {
      row[c] = block[(r * (uint8_t)k_ra8_jpeg_block_dim) + c];
    }
    fwd_dct_1d_norm(row, out);
    for (uint8_t c = 0U; c < (uint8_t)k_ra8_jpeg_block_dim; c++) {
      tmp[(r * (uint8_t)k_ra8_jpeg_block_dim) + c] = out[c];
    }
  }
  for (uint8_t c = 0U; c < (uint8_t)k_ra8_jpeg_block_dim; c++) {
    for (uint8_t r = 0U; r < (uint8_t)k_ra8_jpeg_block_dim; r++) {
      col[r] = tmp[(r * (uint8_t)k_ra8_jpeg_block_dim) + c];
    }
    fwd_dct_1d_norm(col, out);
    for (uint8_t r = 0U; r < (uint8_t)k_ra8_jpeg_block_dim; r++) {
      block[(r * (uint8_t)k_ra8_jpeg_block_dim) + c] = out[r];
    }
  }
}

/* ------------------------------------------------------------------ */
/* Quantization */
/* ------------------------------------------------------------------ */

/**
 * @brief Compute the IJG quality-scale factor.
 *
 * @details Maps the user quality 1..100 onto the IJG percentage scale:
 *          `5000/q` below the pivot (50), `200 - 2q` at or above it.
 *
 * @param[in] q Quality setting (1..100, caller-validated).
 *
 * @return Scale percentage applied to the Annex K.1 base tables.
 * @retval 5000..100 For `q` in 1..49.
 * @retval 100..0    For `q` in 50..100.
 *
 * @pre `q >= 1` (public API validated the range).
 * @pre `q <= 100`.
 * @post Return value matches the IJG `jpeg_quality_scaling()` curve.
 * @post No state is mutated.
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
static uint16_t enc_quality_scale(uint8_t q)
{
  if (q < (uint8_t)k_ra8_jpeg_quality_pivot) {
    return (uint16_t)(k_jpeg_q_scale_low / q);
  }
  return (uint16_t)(k_jpeg_q_scale_high - (2U * (uint16_t)q));
}

/**
 * @brief Scale a base quantization table by `scale/100`, clamped to 1..255.
 *
 * @details Applies the IJG rounding form `(base*scale + 50) / 100` per
 *          entry and clamps into the legal 8-bit DQT range.
 *
 * @param[in]  base  64-entry Annex K.1 base table.
 * @param[out] dst   64-entry destination table.
 * @param[in]  scale Percentage from `enc_quality_scale()`.
 *
 * @pre `base` and `dst` hold 64 valid/writable bytes each.
 * @pre `scale <= 5000` (bounded by the quality curve).
 * @post Every `dst` entry is in 1..255.
 * @post No global state is touched.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static void enc_scale_qtab(const uint8_t* base, uint8_t* dst, uint16_t scale)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_jpeg_block_size; i++) {
    uint32_t t = (((uint32_t)base[i] * (uint32_t)scale) + k_jpeg_q_round_bias) / k_jpeg_q_percent;
    if (t < 1U) {
      t = 1U;
    } else if (t > (uint32_t)k_ra8_jpeg_pixel_max) {
      t = (uint32_t)k_ra8_jpeg_pixel_max;
    }
    dst[i] = (uint8_t)t;
  }
}

/**
 * @brief Compute the number of significant-magnitude bits of `v`.
 *
 * @details T.81 Table F.1 SSSS category: bit length of `|v|`, with 0
 *          mapping to category 0.
 *
 * @param[in] v Coefficient difference or value.
 *
 * @return Magnitude category (bit count of `|v|`).
 * @retval 0     `v` is zero.
 * @retval 1..31 Bit length of `|v|` otherwise.
 *
 * @pre `v > INT32_MIN` (JPEG coefficients are far smaller).
 * @pre Caller uses the result as a Huffman symbol or SSSS nibble.
 * @post Return value is the exact bit length of `|v|`.
 * @post No state is mutated.
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
static uint8_t enc_bits_needed(int32_t v)
{
  uint32_t a = (v < 0) ? (uint32_t)(-v) : (uint32_t)v;
  uint8_t  n = 0U;
  while (a != 0U) {
    a >>= 1U;
    n++;
  }
  return n;
}

/**
 * @brief Quantize one DCT coefficient with symmetric rounding.
 *
 * @details Divides by the quant-table entry with round-half-away-from-
 *          zero semantics; a zero table entry is clamped to 1 as a
 *          divide-by-zero guard.
 *
 * @param[in] v  Raw DCT coefficient.
 * @param[in] qv Quant-table divisor entry.
 *
 * @return Quantized coefficient.
 * @retval 0        `|v|` below half the divisor.
 * @retval non-zero Rounded quotient otherwise.
 *
 * @pre `qv >= 1` in practice (`enc_scale_qtab()` clamps to 1).
 * @pre `v` is a valid 2-D DCT output value.
 * @post Sign of the result matches the sign of `v`.
 * @post No state is mutated.
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
static int32_t enc_quantize(int32_t v, uint8_t qv)
{
  int32_t q = (int32_t)qv;
  if (q == 0) {
    q = 1;
  }
  if (v < 0) {
    int32_t a = -v + (q >> 1);
    return -(a / q);
  }
  return (v + (q >> 1)) / q;
}

/* ------------------------------------------------------------------ */
/* Per-block Huffman emission */
/* ------------------------------------------------------------------ */

/**
 * @brief Emit the 63 AC coefficients of one quantized block.
 *
 * @details
 * T.81 sec F.1.2.2 run-length coding: zero runs of 16 emit ZRL
 * (0xF0), each non-zero coefficient emits its (RRRR,SSSS) symbol
 * followed by the magnitude bits, and a trailing zero run emits EOB.
 *
 * @param[in,out] e        Encoder context (bit buffer mutates).
 * @param[in]     z        64-entry zig-zag-ordered quantized block.
 * @param[in]     ac_codes AC Huffman code LUT for this component.
 * @param[in]     ac_sizes AC Huffman code-length LUT.
 *
 * @pre `z` was produced by the caller's quantize loop.
 * @pre The AC LUTs were built by `ra8_jpeg_sw_priv_enc_build_huff()`.
 * @post All non-zero AC coefficients are in the entropy stream.
 * @post An EOB symbol terminates the block when it ends in zeros.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static void enc_block_ac(ra8_jpeg_enc_ctx_t* e,
                         const int32_t*      z,
                         const uint16_t*     ac_codes,
                         const uint8_t*      ac_sizes)
{
  uint8_t run = 0U;
  for (uint8_t k = 1U; k < (uint8_t)k_ra8_jpeg_block_size; k++) {
    if (z[k] == 0) {
      run++;
      continue;
    }
    while (run >= 16U) {
      /* ZRL = 0xF0. */
      ra8_jpeg_sw_priv_enc_put_bits(e,
                                    (uint32_t)ac_codes[k_jpeg_zrl_symbol],
                                    ac_sizes[k_jpeg_zrl_symbol]);
      run = (uint8_t)(run - 16U);
    }
    uint8_t mb  = enc_bits_needed(z[k]);
    uint8_t sym = (uint8_t)((run << k_ra8_jpeg_nibble_shift) | mb);
    ra8_jpeg_sw_priv_enc_put_bits(e, (uint32_t)ac_codes[sym], ac_sizes[sym]);
    int32_t v = (z[k] < 0) ? (z[k] - 1) : z[k];
    ra8_jpeg_sw_priv_enc_put_bits(e, (uint32_t)v & ((1U << mb) - 1U), mb);
    run = 0U;
  }
  if (run > 0U) {
    /* EOB = 0x00. */
    ra8_jpeg_sw_priv_enc_put_bits(e, (uint32_t)ac_codes[0U], ac_sizes[0U]);
  }
}

/**
 * @brief Encode one 8x8 block of source samples.
 *
 * @details Forward-DCTs the spatial block, quantizes it in zig-zag
 *          order, emits the DC difference (T.81 sec F.1.2.1) and hands
 *          the AC coefficients to `enc_block_ac()`.
 *
 * @param[in,out] e        Encoder context (DC predictor + bits mutate).
 * @param[in]     spatial  64-entry level-shifted sample block.
 * @param[in]     qtab     Scaled quant table for this component.
 * @param[in]     comp_idx Component index (0 luma, 1 Cb, 2 Cr).
 * @param[in]     dc_codes DC Huffman code LUT.
 * @param[in]     dc_sizes DC Huffman code-length LUT.
 * @param[in]     ac_codes AC Huffman code LUT.
 * @param[in]     ac_sizes AC Huffman code-length LUT.
 *
 * @pre All LUTs were built by `ra8_jpeg_sw_priv_enc_build_huff()`.
 * @pre `spatial` holds 64 valid samples.
 * @post The block's entropy bits are in the output stream.
 * @post `e->dc_pred[comp_idx]` reflects this block's DC value.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static void enc_block(ra8_jpeg_enc_ctx_t* e,
                      const int32_t*      spatial,
                      const uint8_t*      qtab,
                      uint8_t             comp_idx,
                      const uint16_t*     dc_codes,
                      const uint8_t*      dc_sizes,
                      const uint16_t*     ac_codes,
                      const uint8_t*      ac_sizes)
{
  int32_t blk[(uint32_t)k_ra8_jpeg_block_size];
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_jpeg_block_size; i++) {
    blk[i] = spatial[i];
  }
  fdct8x8(blk);

  /* Quantize in zig-zag order. */
  int32_t z[(uint32_t)k_ra8_jpeg_block_size];
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_jpeg_block_size; i++) {
    z[i] = enc_quantize(blk[s_zigzag[i]], qtab[s_zigzag[i]]);
  }

  /* DC differential. */
  int32_t diff         = z[0] - e->dc_pred[comp_idx];
  e->dc_pred[comp_idx] = z[0];
  uint8_t nb           = enc_bits_needed(diff);
  ra8_jpeg_sw_priv_enc_put_bits(e, (uint32_t)dc_codes[nb], dc_sizes[nb]);
  if (nb != 0U) {
    int32_t v = (diff < 0) ? (diff - 1) : diff;
    ra8_jpeg_sw_priv_enc_put_bits(e, (uint32_t)v & ((1U << nb) - 1U), nb);
  }

  /* AC. */
  enc_block_ac(e, z, ac_codes, ac_sizes);
}

/* ------------------------------------------------------------------ */
/* RGB -> YCbCr conversion and block sampling */
/* ------------------------------------------------------------------ */

/**
 * @brief Convert one row of RGB into Y, Cb, Cr samples (BT.601 Q16).
 *
 * @details Fixed-point BT.601 forward transform using the same Q16
 *          coefficients as IJG `jccolor.c`; chroma outputs carry the
 *          +128 level offset.
 *
 * @param[in]  rgb Packed RGB888 row (`n` pixels).
 * @param[in]  n   Pixel count.
 * @param[out] y   Luma output row.
 * @param[out] cb  Cb output row (level-offset applied).
 * @param[out] cr  Cr output row (level-offset applied).
 *
 * @pre All five pointers are non-NULL (module-internal call chain).
 * @pre Output rows hold at least `n` int32 slots each.
 * @post Every output sample is in the nominal 0..255 range.
 * @post No global state is touched.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static void enc_rgb_to_ycc_row(const uint8_t* rgb, uint16_t n, int32_t* y, int32_t* cb, int32_t* cr)
{
  for (uint16_t i = 0U; i < n; i++) {
    uint32_t px = (uint32_t)i * (uint32_t)k_ra8_jpeg_rgb_components;
    int32_t  r  = (int32_t)rgb[px];
    int32_t  g  = (int32_t)rgb[px + 1U];
    int32_t  b  = (int32_t)rgb[px + 2U];
    y[i]        = (((int32_t)k_ra8_jpeg_yr * r) + ((int32_t)k_ra8_jpeg_yg * g) +
                   ((int32_t)k_ra8_jpeg_yb * b)) >>
                  k_ra8_jpeg_yuv_shift;
    cb[i]       = ((((int32_t)k_ra8_jpeg_cbr * r) + ((int32_t)k_ra8_jpeg_cbg * g) +
                    ((int32_t)k_ra8_jpeg_cbb * b)) >>
                   k_ra8_jpeg_yuv_shift) +
                  (int32_t)k_ra8_jpeg_level_offset;
    cr[i]       = ((((int32_t)k_ra8_jpeg_crr * r) + ((int32_t)k_ra8_jpeg_crg * g) +
                    ((int32_t)k_ra8_jpeg_crb * b)) >>
                   k_ra8_jpeg_yuv_shift) +
                  (int32_t)k_ra8_jpeg_level_offset;
  }
}

/**
 * @brief Sample one 8x8 luma block from the Y plane (clamp-to-edge).
 *
 * @details Reads an 8x8 window at (x0, y0), clamping out-of-range
 *          coordinates to the plane edge, and level-shifts each sample
 *          by -128 for the DCT.
 *
 * @param[in]  yplane  Luma plane (`plane_w` samples per row).
 * @param[in]  plane_w Plane width in samples.
 * @param[in]  plane_h Plane height in rows.
 * @param[in]  x0      Window left edge.
 * @param[in]  y0      Window top edge.
 * @param[out] out     64-entry destination block.
 *
 * @pre `yplane` covers `plane_w * plane_h` samples.
 * @pre `plane_w >= 1` and `plane_h >= 1`.
 * @post `out` holds 64 level-shifted samples.
 * @post No global state is touched.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static void enc_sample_y_block(const int32_t* yplane,
                               uint16_t       plane_w,
                               uint16_t       plane_h,
                               uint16_t       x0,
                               uint16_t       y0,
                               int32_t*       out)
{
  for (uint8_t r = 0U; r < (uint8_t)k_ra8_jpeg_block_dim; r++) {
    uint16_t sy = (uint16_t)(y0 + r);
    if (sy >= plane_h) {
      sy = (uint16_t)(plane_h - 1U);
    }
    for (uint8_t c = 0U; c < (uint8_t)k_ra8_jpeg_block_dim; c++) {
      uint16_t sx = (uint16_t)(x0 + c);
      if (sx >= plane_w) {
        sx = (uint16_t)(plane_w - 1U);
      }
      out[(r * (uint8_t)k_ra8_jpeg_block_dim) + c] =
        yplane[((uint32_t)sy * plane_w) + sx] - (int32_t)k_ra8_jpeg_level_offset;
    }
  }
}

/**
 * @brief Average the 2x2 chroma neighbourhood at (sx, sy), clamped.
 *
 * @details
 * Sums the four plane samples of the 2x2 window whose top-left corner
 * is (sx, sy), clamping each coordinate to the plane edge, and
 * returns the integer mean -- the 4:2:0 sub-sampling kernel of
 * `enc_sample_c_block_420()`.
 *
 * @param[in] cplane  Chroma plane (`plane_w` samples per row).
 * @param[in] plane_w Plane width in samples.
 * @param[in] plane_h Plane height in rows.
 * @param[in] sx      Window left edge.
 * @param[in] sy      Window top edge.
 *
 * @return Integer mean of the four (edge-clamped) samples.
 * @retval 0..255 For chroma planes in the nominal range.
 *
 * @pre `cplane` covers `plane_w * plane_h` samples.
 * @pre `plane_w >= 1` and `plane_h >= 1`.
 * @post No global state is touched.
 * @post The divisor is exactly 4 (the window never shrinks).
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static int32_t
enc_avg_2x2(const int32_t* cplane, uint16_t plane_w, uint16_t plane_h, uint16_t sx, uint16_t sy)
{
  int32_t s = 0;
  uint8_t n = 0U;
  for (uint8_t dy = 0U; dy < 2U; dy++) {
    for (uint8_t dx = 0U; dx < 2U; dx++) {
      uint16_t yy = (uint16_t)(sy + dy);
      uint16_t xx = (uint16_t)(sx + dx);
      if (yy >= plane_h) {
        yy = (uint16_t)(plane_h - 1U);
      }
      if (xx >= plane_w) {
        xx = (uint16_t)(plane_w - 1U);
      }
      s += cplane[((uint32_t)yy * plane_w) + xx];
      n++;
    }
  }
  return s / (int32_t)n;
}

/**
 * @brief Build one 8x8 chroma block by 2x2 averaging from a 16x16 region.
 *
 * @details Each destination sample is the `enc_avg_2x2()` mean of the
 *          matching 2x2 source window, level-shifted by -128 for the
 *          DCT -- the 4:2:0 chroma sub-sampling step.
 *
 * @param[in]  cplane  Chroma plane (`plane_w` samples per row).
 * @param[in]  plane_w Plane width in samples.
 * @param[in]  plane_h Plane height in rows.
 * @param[in]  x0      Source region left edge.
 * @param[in]  y0      Source region top edge.
 * @param[out] out     64-entry destination block.
 *
 * @pre `cplane` covers `plane_w * plane_h` samples.
 * @pre `plane_w >= 1` and `plane_h >= 1`.
 * @post `out` holds 64 level-shifted sub-sampled chroma values.
 * @post No global state is touched.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static void enc_sample_c_block_420(const int32_t* cplane,
                                   uint16_t       plane_w,
                                   uint16_t       plane_h,
                                   uint16_t       x0,
                                   uint16_t       y0,
                                   int32_t*       out)
{
  for (uint8_t r = 0U; r < (uint8_t)k_ra8_jpeg_block_dim; r++) {
    for (uint8_t c = 0U; c < (uint8_t)k_ra8_jpeg_block_dim; c++) {
      uint16_t sy = (uint16_t)(y0 + (r * 2U));
      uint16_t sx = (uint16_t)(x0 + (c * 2U));
      out[(r * (uint8_t)k_ra8_jpeg_block_dim) + c] =
        enc_avg_2x2(cplane, plane_w, plane_h, sx, sy) - (int32_t)k_ra8_jpeg_level_offset;
    }
  }
}

/**
 * @brief Convert 16 source RGB rows into Y / Cb / Cr 16-row strips.
 *
 * @details
 * For each of 16 output rows, replicates the right-edge / bottom-edge
 * source pixels (clamp-to-edge) so that the YCbCr strips cover the full
 * `pad_w` width and the requested vertical strip starting at `mby`.
 * Uses the caller-provided scratch RGB buffer for one row at a time, so
 * the chroma sub-sampling later in `enc_sample_c_block_420()` sees
 * fully-populated planes.
 *
 * @param[in]  rgb      Source RGB888 image of size w*h.
 * @param[in]  w        Source image width in pixels.
 * @param[in]  h        Source image height in pixels.
 * @param[in]  pad_w    Padded (16-aligned) strip width.
 * @param[in]  mby      Strip start row in the padded image.
 * @param[out] y_strip  Output Y plane of pad_w * 16 samples.
 * @param[out] cb_strip Output Cb plane of pad_w * 16 samples.
 * @param[out] cr_strip Output Cr plane of pad_w * 16 samples.
 * @param[out] tmp_rgb  Scratch RGB row buffer of pad_w*3 bytes.
 *
 * @pre `rgb`, `y_strip`, `cb_strip`, `cr_strip`, `tmp_rgb` non-NULL.
 * @pre `w > 0`, `h > 0`, `pad_w >= w`, `pad_w` is a multiple of 16.
 * @post YCbCr strips populated for all 16 rows of the slice.
 * @post `tmp_rgb` contents undefined after return (scratch only).
 *
 * @note Not thread-safe; relies on caller-provided scratch buffers.
 * @since 0.1.0
 */
static void enc_convert_strip_to_ycc(const uint8_t* rgb,
                                     uint16_t       w,
                                     uint16_t       h,
                                     uint16_t       pad_w,
                                     uint16_t       mby,
                                     int32_t*       y_strip,
                                     int32_t*       cb_strip,
                                     int32_t*       cr_strip,
                                     uint8_t*       tmp_rgb)
{
  for (uint8_t r = 0U; r < 16U; r++) {
    uint16_t src_y = mby + r;
    if (src_y >= h) {
      src_y = (uint16_t)(h - 1U);
    }
    for (uint16_t c = 0U; c < pad_w; c++) {
      uint16_t src_x = c;
      if (src_x >= w) {
        src_x = (uint16_t)(w - 1U);
      }
      uint32_t sidx =
        (((uint32_t)src_y * (uint32_t)w) + (uint32_t)src_x) * (uint32_t)k_ra8_jpeg_rgb_components;
      uint32_t didx      = (uint32_t)c * (uint32_t)k_ra8_jpeg_rgb_components;
      tmp_rgb[didx]      = rgb[sidx + 0U];
      tmp_rgb[didx + 1U] = rgb[sidx + 1U];
      tmp_rgb[didx + 2U] = rgb[sidx + 2U];
    }
    enc_rgb_to_ycc_row(tmp_rgb,
                       pad_w,
                       &y_strip[(size_t)r * pad_w],
                       &cb_strip[(size_t)r * pad_w],
                       &cr_strip[(size_t)r * pad_w]);
  }
}

/**
 * @brief Encode every 16x16 MCU in a 16-row YCbCr strip.
 *
 * @details
 * For each MCU column inside `pad_w`, samples four 8x8 luma blocks at
 * (0,0)(8,0)(0,8)(8,8), then two sub-sampled 8x8 chroma blocks (Cb,Cr).
 * Each block is fed through `enc_block()` with the matching quant table
 * and Huffman LUTs, which emits the entropy-coded bits into the encoder
 * context.
 *
 * @param[in,out] e        Encoder context (bit-buffer mutates).
 * @param[in]     pad_w    Padded strip width (multiple of 16).
 * @param[in]     y_strip  Luma plane samples for this strip.
 * @param[in]     cb_strip Cb plane samples for this strip.
 * @param[in]     cr_strip Cr plane samples for this strip.
 *
 * @pre `e` is initialized with valid Huffman LUTs.
 * @pre `pad_w > 0` and a multiple of 16.
 * @post `e->pos`/bit buffer advanced by the entropy bytes of every MCU in the strip.
 * @post Block sample order is byte-identical to the pre-split encoder.
 *
 * @note Not thread-safe; caller serializes via encoder context.
 * @since 0.1.0
 */
static void enc_encode_mcu_row(ra8_jpeg_enc_ctx_t* e,
                               uint16_t            pad_w,
                               const int32_t*      y_strip,
                               const int32_t*      cb_strip,
                               const int32_t*      cr_strip)
{
  for (uint16_t mbx = 0U; mbx < pad_w; mbx = (uint16_t)(mbx + 16U)) {
    int32_t blk[(uint32_t)k_ra8_jpeg_block_size];
    /* 4 luma blocks: (0,0) (0,8) (8,0) (8,8). */
    for (uint8_t by = 0U; by < 2U; by++) {
      for (uint8_t bx = 0U; bx < 2U; bx++) {
        enc_sample_y_block(y_strip,
                           pad_w,
                           16U,
                           (uint16_t)(mbx + (bx * 8U)),
                           (uint16_t)(by * 8U),
                           blk);
        enc_block(e, blk, e->qy, 0U, e->code_dc_l, e->size_dc_l, e->code_ac_l, e->size_ac_l);
      }
    }
    enc_sample_c_block_420(cb_strip, pad_w, 16U, mbx, 0U, blk);
    enc_block(e, blk, e->qc, 1U, e->code_dc_c, e->size_dc_c, e->code_ac_c, e->size_ac_c);
    enc_sample_c_block_420(cr_strip, pad_w, 16U, mbx, 0U, blk);
    enc_block(e, blk, e->qc, 2U, e->code_dc_c, e->size_dc_c, e->code_ac_c, e->size_ac_c);
  }
}

/* ------------------------------------------------------------------ */
/* Top-level driver and public API */
/* ------------------------------------------------------------------ */

/**
 * @brief Top-level encode driver -- emits the full JFIF byte stream.
 *
 * @details Builds the quant tables and Huffman LUTs, emits the header
 *          segments, streams the image through 16-row YCbCr strips
 *          (bounded working set), then flushes the entropy bits and
 *          closes the stream with EOI.
 *
 * @param[in,out] e       Zeroed encoder context bound to the output buffer.
 * @param[in]     rgb     Source RGB888 image.
 * @param[in]     w       Image width in pixels.
 * @param[in]     h       Image height in pixels.
 * @param[in]     quality IJG quality setting (1..100).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Stream complete in `e->dst[0..e->pos)`.
 * @retval k_ra8_err_invalid_arg  Padded width exceeds the encoder cap.
 * @retval k_ra8_err_invalid_size Output buffer overflowed.
 *
 * @pre `e`, `rgb` are non-NULL (public API validated them).
 * @pre `w`, `h`, `quality` were range-checked by the public API.
 * @post On success `e->pos` is the total stream length.
 * @post On overflow `e->overflow` is latched and an error returned.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t
enc_run(ra8_jpeg_enc_ctx_t* e, const uint8_t* rgb, uint16_t w, uint16_t h, uint8_t quality)
{
  /* Build quantization tables. */
  uint16_t qs = enc_quality_scale(quality);
  enc_scale_qtab(s_quant_luma, e->qy, qs);
  enc_scale_qtab(s_quant_chroma, e->qc, qs);

  /* Build Huffman code tables from K.3.3 BITS/VALUES. */
  uint16_t total_dc_l;
  uint16_t total_ac_l;
  uint16_t total_dc_c;
  uint16_t total_ac_c;
  ra8_jpeg_sw_priv_enc_build_huff(e, &total_dc_l, &total_ac_l, &total_dc_c, &total_ac_c);

  ra8_jpeg_sw_priv_enc_headers(e, w, h, total_dc_l, total_ac_l, total_dc_c, total_ac_c);

  /* Convert source rows to YCbCr 16-row strips. Static buffers
   * keep the host stack small while still respecting NASA Rule 3
   * (no heap). The encoder is single-threaded, so file-scope
   * sharing is safe. */
  uint16_t pad_w = (uint16_t)((w + k_jpeg_mcu_align) & ~k_jpeg_mcu_align);
  uint16_t pad_h = (uint16_t)((h + k_jpeg_mcu_align) & ~k_jpeg_mcu_align);
  if ((uint32_t)pad_w > (uint32_t)k_ra8_jpeg_enc_max_w) {
    return k_ra8_err_invalid_arg;
  }
  static int32_t s_y_strip[16U * (uint32_t)k_ra8_jpeg_enc_max_w];
  static int32_t s_cb_strip[16U * (uint32_t)k_ra8_jpeg_enc_max_w];
  static int32_t s_cr_strip[16U * (uint32_t)k_ra8_jpeg_enc_max_w];
  static uint8_t s_tmp_rgb[3U * (uint32_t)k_ra8_jpeg_enc_max_w];

  for (uint16_t mby = 0U; mby < pad_h; mby = (uint16_t)(mby + 16U)) {
    enc_convert_strip_to_ycc(rgb, w, h, pad_w, mby, s_y_strip, s_cb_strip, s_cr_strip, s_tmp_rgb);
    enc_encode_mcu_row(e, pad_w, s_y_strip, s_cb_strip, s_cr_strip);
  }

  ra8_jpeg_sw_priv_enc_flush_bits(e);
  ra8_jpeg_sw_priv_enc_emit_u16(e, (uint16_t)k_ra8_jpeg_marker_eoi);

  if (e->overflow) {
    return k_ra8_err_invalid_size;
  }
  return k_ra8_ok;
}

ra8_err_t ra8_jpeg_sw_encode(const uint8_t* rgb_buf,
                             uint16_t       width,
                             uint16_t       height,
                             uint8_t        quality,
                             uint8_t*       out_buf,
                             uint32_t       out_buf_len,
                             uint32_t*      out_len)
{
  RA8_CHECK_NULL_PTR(rgb_buf, s_tag, "rgb_buf is NULL");
  RA8_CHECK_NULL_PTR(out_buf, s_tag, "out_buf is NULL");
  RA8_CHECK_NULL_PTR(out_len, s_tag, "out_len is NULL");
  *out_len = 0U;
  if (width == 0U || height == 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (quality < (uint8_t)k_ra8_jpeg_sw_quality_min ||
      quality > (uint8_t)k_ra8_jpeg_sw_quality_max) {
    return k_ra8_err_invalid_arg;
  }

  /* Encoder context contains 2KiB of Huffman code/size LUTs; allocate
   * static to avoid the project's stack-usage budget firing. */
  static ra8_jpeg_enc_ctx_t s_e;
  ra8_jpeg_enc_ctx_t*       e = &s_e;
  memset(e, 0, sizeof(*e));
  e->dst = out_buf;
  e->cap = out_buf_len;

  ra8_err_t err = enc_run(e, rgb_buf, width, height, quality);
  if (err != k_ra8_ok) {
    return err;
  }
  *out_len = e->pos;
  return k_ra8_ok;
}
