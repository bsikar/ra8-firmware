/**
 * @file ra_jpeg_sw_encode.c
 * @brief Pure-software baseline JPEG encoder implementation.
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Implements `ra_jpeg_sw_encode()` for baseline (8-bit, sequential,
 * Huffman) JPEG output in the YCbCr 4:2:0 layout. The reference
 * codec is the `write_JPEG_file()` example shipped with the IJG
 * libjpeg sources and the matching pseudo-code in ITU-T T.81 Annex
 * K; it has been re-implemented here from scratch in this project's
 * style and naming conventions, with no third-party code copied.
 *
 * This is the encoder half of the software JPEG codec; the decoder
 * half and the shared entropy/DSP primitives live in `ra_jpeg_sw.c`,
 * and every cross-unit symbol lives in `ra_jpeg_sw_internal.h`.
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

#include "ra_check.h"
#include "ra_err.h"
#include "ra_jpeg_sw.h"
#include "ra_jpeg_sw_internal.h"
#include "ra_log.h"

/* The marker codes, Huffman BITS/HUFFVAL tables and quantization
 * tables in this file are taken verbatim from ITU-T T.81 / ISO 10918-1
 * Annex K. The state-machine functions (`enc_run`, `enc_block`) are
 * above clang-tidy's default function-size and cognitive-complexity
 * thresholds because each step of the JPEG codec maps 1:1 onto the
 * spec text -- splitting them further would obscure the spec citations
 * rather than help readability. The `readability-magic-numbers`
 * suppression covers spec-defined byte values such as the marker codes
 * and JFIF segment-length constants; each occurrence carries a
 * `T.81 sec X.Y` citation. The `readability-redundant-casting`
 * suppression covers the `(uint8_t)k_ra_jpeg_*_t` form used to make
 * enum membership explicit at use sites that mix multiple enum
 * families. */
// NOLINTBEGIN(readability-function-size,readability-function-cognitive-complexity,readability-redundant-casting,readability-math-missing-parentheses,bugprone-implicit-widening-of-multiplication-result,clang-analyzer-core.DivideZero,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)

/** @brief Component log tag. */
static const char* s_tag = "JPEG_SW";

/* ------------------------------------------------------------------ */
/* Encoder quantization and Huffman reference tables */
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

/* ----- Annex K.3.3 reference Huffman tables (luma DC/AC, chroma DC/AC) ----- */

/** @brief K.3.3 luma DC BITS (number of codes of each length 1..16). */
static const uint8_t s_hbits_dc_luma[16] = {
  0,
  1,
  5,
  1,
  1,
  1,
  1,
  1,
  1,
  0,
  0,
  0,
  0,
  0,
  0,
  0,
};
/** @brief K.3.3 luma DC HUFFVAL (12 symbols). */
static const uint8_t s_hval_dc_luma[12] = {
  0,
  1,
  2,
  3,
  4,
  5,
  6,
  7,
  8,
  9,
  10,
  11,
};

/** @brief K.3.3 chroma DC BITS. */
static const uint8_t s_hbits_dc_chroma[16] = {
  0,
  3,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  1,
  0,
  0,
  0,
  0,
  0,
};
/** @brief K.3.3 chroma DC HUFFVAL. */
static const uint8_t s_hval_dc_chroma[12] = {
  0,
  1,
  2,
  3,
  4,
  5,
  6,
  7,
  8,
  9,
  10,
  11,
};

/** @brief K.3.3 luma AC BITS. */
static const uint8_t s_hbits_ac_luma[16] = {
  0,
  2,
  1,
  3,
  3,
  2,
  4,
  3,
  5,
  5,
  4,
  4,
  0,
  0,
  1,
  0x7d,
};
/** @brief K.3.3 luma AC HUFFVAL (162 symbols). */
static const uint8_t s_hval_ac_luma[162] = {
  0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61,
  0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52,
  0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25,
  0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
  0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64,
  0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83,
  0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
  0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
  0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3,
  0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8,
  0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

/** @brief K.3.3 chroma AC BITS. */
static const uint8_t s_hbits_ac_chroma[16] = {
  0,
  2,
  1,
  2,
  4,
  4,
  3,
  4,
  7,
  5,
  4,
  4,
  0,
  1,
  2,
  0x77,
};
/** @brief K.3.3 chroma AC HUFFVAL. */
static const uint8_t s_hval_ac_chroma[162] = {
  0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61,
  0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33,
  0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18,
  0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
  0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63,
  0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a,
  0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
  0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
  0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
  0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7,
  0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

/* ------------------------------------------------------------------ */
/* Forward DCT (encoder, AAN scaled) */
/* ------------------------------------------------------------------ */

/* fwd dct 1d norm -- see surrounding code and HUM citations. */
static void fwd_dct_1d_norm(const int32_t* in, int32_t* out)
{
  for (uint8_t k = 0U; k < (uint8_t)k_ra_jpeg_block_dim; k++) {
    int64_t s = 0;
    for (uint8_t n = 0U; n < (uint8_t)k_ra_jpeg_block_dim; n++) {
      s += (int64_t)in[n] * (int64_t)s_dct_cos_q14[k][n];
    }
    /* s is Q14*Q0 = Q14; multiply by Q14 weight -> Q28; round to Q0. */
    int64_t r =
      (s * (int64_t)s_dct_w_q14[k] + (1LL << k_jpeg_idct_p1_bias_sh)) >> k_jpeg_idct_p1_sh;
    out[k] = (int32_t)r;
  }
}

/* 2-D forward DCT, in place, with normalization folded in -- see surrounding code and HUM citations. */
static void fdct8x8(int32_t* block)
{
  int32_t tmp[(uint32_t)k_ra_jpeg_block_size];
  int32_t row[(uint32_t)k_ra_jpeg_block_dim];
  int32_t col[(uint32_t)k_ra_jpeg_block_dim];
  int32_t out[(uint32_t)k_ra_jpeg_block_dim];
  for (uint8_t r = 0U; r < (uint8_t)k_ra_jpeg_block_dim; r++) {
    for (uint8_t c = 0U; c < (uint8_t)k_ra_jpeg_block_dim; c++) {
      row[c] = block[r * (uint8_t)k_ra_jpeg_block_dim + c];
    }
    fwd_dct_1d_norm(row, out);
    for (uint8_t c = 0U; c < (uint8_t)k_ra_jpeg_block_dim; c++) {
      tmp[r * (uint8_t)k_ra_jpeg_block_dim + c] = out[c];
    }
  }
  for (uint8_t c = 0U; c < (uint8_t)k_ra_jpeg_block_dim; c++) {
    for (uint8_t r = 0U; r < (uint8_t)k_ra_jpeg_block_dim; r++) {
      col[r] = tmp[r * (uint8_t)k_ra_jpeg_block_dim + c];
    }
    fwd_dct_1d_norm(col, out);
    for (uint8_t r = 0U; r < (uint8_t)k_ra_jpeg_block_dim; r++) {
      block[r * (uint8_t)k_ra_jpeg_block_dim + c] = out[r];
    }
  }
}

/* ================================================================== */
/* Encoder */
/* ================================================================== */

/**
 * @struct ra_jpeg_enc_ctx_t
 * @brief Encoder state -- buffer cursor, bit accumulator, scaled
 *        quant tables, DC predictors.
 */
typedef struct {
  uint8_t* dst;
  uint32_t cap;
  uint32_t pos;

  uint32_t bit_buf;
  uint8_t  bit_cnt;

  uint8_t qy[k_ra_jpeg_block_size];
  uint8_t qc[k_ra_jpeg_block_size];

  int32_t dc_pred[k_ra_jpeg_max_comps];

  uint16_t code_dc_l[k_ra_jpeg_huff_max];
  uint8_t  size_dc_l[k_ra_jpeg_huff_max];
  uint16_t code_ac_l[k_ra_jpeg_huff_max];
  uint8_t  size_ac_l[k_ra_jpeg_huff_max];
  uint16_t code_dc_c[k_ra_jpeg_huff_max];
  uint8_t  size_dc_c[k_ra_jpeg_huff_max];
  uint16_t code_ac_c[k_ra_jpeg_huff_max];
  uint8_t  size_ac_c[k_ra_jpeg_huff_max];
  bool     overflow;
} ra_jpeg_enc_ctx_t;

/* Append one byte; sets `overflow` on capacity exhaustion -- see surrounding code and HUM citations. */
static void enc_emit_u8(ra_jpeg_enc_ctx_t* e, uint8_t b)
{
  if (e->pos >= e->cap) {
    e->overflow = true;
    return;
  }
  e->dst[e->pos] = b;
  e->pos++;
}

/* Append a 16-bit big-endian word -- see surrounding code and HUM citations. */
static void enc_emit_u16(ra_jpeg_enc_ctx_t* e, uint16_t v)
{
  enc_emit_u8(e, (uint8_t)(v >> k_ra_jpeg_byte_shift));
  enc_emit_u8(e, (uint8_t)(v & k_jpeg_byte_mask));
}

/* Push `n` bits MSB-first into the entropy stream with stuffing -- see surrounding code and HUM citations. */
static void enc_put_bits(ra_jpeg_enc_ctx_t* e, uint32_t code, uint8_t n)
{
  e->bit_buf = (e->bit_buf << n) | (code & ((1U << n) - 1U));
  e->bit_cnt = (uint8_t)(e->bit_cnt + n);
  while (e->bit_cnt >= (uint8_t)k_ra_jpeg_byte_shift) {
    uint8_t b = (uint8_t)(e->bit_buf >> (e->bit_cnt - k_ra_jpeg_byte_shift));
    enc_emit_u8(e, b);
    if (b == (uint8_t)k_ra_jpeg_marker_byte) {
      enc_emit_u8(e, 0U);
    }
    e->bit_cnt = (uint8_t)(e->bit_cnt - k_ra_jpeg_byte_shift);
  }
}

/* Flush any partial byte at the end of a scan with 1-fill -- see surrounding code and HUM citations. */
static void enc_flush_bits(ra_jpeg_enc_ctx_t* e)
{
  if (e->bit_cnt > 0U) {
    uint32_t pad = (1U << (k_ra_jpeg_byte_shift - e->bit_cnt)) - 1U;
    enc_put_bits(e,
                 ((e->bit_buf << (k_ra_jpeg_byte_shift - e->bit_cnt)) | pad) & k_jpeg_byte_mask,
                 (uint8_t)(k_ra_jpeg_byte_shift - e->bit_cnt));
  }
}

/* Build the canonical-code table from BITS+HUFFVAL -- see surrounding code and HUM citations. */
static void enc_build_codes(const uint8_t* bits,
                            const uint8_t* vals,
                            uint16_t*      codes,
                            uint8_t*       sizes,
                            uint16_t       total)
{
  uint8_t  huffsize[k_ra_jpeg_huff_max + 1U];
  uint16_t k = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra_jpeg_huff_lengths; i++) {
    for (uint8_t j = 0U; j < bits[i]; j++) {
      huffsize[k] = (uint8_t)(i + 1U);
      k++;
    }
  }
  huffsize[k] = 0U;

  uint16_t huffcode[k_ra_jpeg_huff_max + 1U];
  uint16_t code = 0U;
  uint8_t  si   = (k > 0U) ? huffsize[0] : 0U;
  uint16_t kk   = 0U;
  while (huffsize[kk] != 0U) {
    while (huffsize[kk] == si) {
      huffcode[kk] = code;
      code++;
      kk++;
    }
    if (huffsize[kk] == 0U) {
      break;
    }
    do {
      code = (uint16_t)(code << 1U);
      si++;
    } while (huffsize[kk] != si);
  }
  for (uint16_t i = 0U; i < total; i++) {
    codes[vals[i]] = huffcode[i];
    sizes[vals[i]] = huffsize[i];
  }
}

/* Compute the IJG quality-scale factor -- see surrounding code and HUM citations. */
static uint16_t enc_quality_scale(uint8_t q)
{
  if (q < (uint8_t)k_ra_jpeg_quality_pivot) {
    return (uint16_t)(k_jpeg_q_scale_low / q);
  }
  return (uint16_t)(k_jpeg_q_scale_high - 2U * (uint16_t)q);
}

/* Scale a base quantization table by `scale_pct/100`, clamp 1 -- see surrounding code and HUM citations. */
static void enc_scale_qtab(const uint8_t* base, uint8_t* dst, uint16_t scale)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra_jpeg_block_size; i++) {
    uint32_t t = ((uint32_t)base[i] * (uint32_t)scale + k_jpeg_q_round_bias) / k_jpeg_q_percent;
    if (t < 1U) {
      t = 1U;
    } else if (t > (uint32_t)k_ra_jpeg_pixel_max) {
      t = (uint32_t)k_ra_jpeg_pixel_max;
    }
    dst[i] = (uint8_t)t;
  }
}

/* Compute number of significant-magnitude bits for `v` -- see surrounding code and HUM citations. */
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

/* Quantize one coeff in zig-zag position `i` -- see surrounding code and HUM citations. */
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

/* Encode one 8x8 block of source samples -- see surrounding code and HUM citations. */
static void enc_block(ra_jpeg_enc_ctx_t* e,
                      const int32_t*     spatial,
                      const uint8_t*     qtab,
                      uint8_t            comp_idx,
                      const uint16_t*    dc_codes,
                      const uint8_t*     dc_sizes,
                      const uint16_t*    ac_codes,
                      const uint8_t*     ac_sizes)
{
  int32_t blk[(uint32_t)k_ra_jpeg_block_size];
  for (uint8_t i = 0U; i < (uint8_t)k_ra_jpeg_block_size; i++) {
    blk[i] = spatial[i];
  }
  fdct8x8(blk);

  /* Quantize in zig-zag order. */
  int32_t z[(uint32_t)k_ra_jpeg_block_size];
  for (uint8_t i = 0U; i < (uint8_t)k_ra_jpeg_block_size; i++) {
    z[i] = enc_quantize(blk[s_zigzag[i]], qtab[s_zigzag[i]]);
  }

  /* DC differential. */
  int32_t diff         = z[0] - e->dc_pred[comp_idx];
  e->dc_pred[comp_idx] = z[0];
  uint8_t nb           = enc_bits_needed(diff);
  enc_put_bits(e, (uint32_t)dc_codes[nb], dc_sizes[nb]);
  if (nb != 0U) {
    int32_t v = (diff < 0) ? (diff - 1) : diff;
    enc_put_bits(e, (uint32_t)v & ((1U << nb) - 1U), nb);
  }

  /* AC. */
  uint8_t run = 0U;
  for (uint8_t k = 1U; k < (uint8_t)k_ra_jpeg_block_size; k++) {
    if (z[k] == 0) {
      run++;
      continue;
    }
    while (run >= 16U) {
      /* ZRL = 0xF0. */
      enc_put_bits(e, (uint32_t)ac_codes[k_jpeg_zrl_symbol], ac_sizes[k_jpeg_zrl_symbol]);
      run = (uint8_t)(run - 16U);
    }
    uint8_t mb  = enc_bits_needed(z[k]);
    uint8_t sym = (uint8_t)((run << k_ra_jpeg_nibble_shift) | mb);
    enc_put_bits(e, (uint32_t)ac_codes[sym], ac_sizes[sym]);
    int32_t v = (z[k] < 0) ? (z[k] - 1) : z[k];
    enc_put_bits(e, (uint32_t)v & ((1U << mb) - 1U), mb);
    run = 0U;
  }
  if (run > 0U) {
    /* EOB = 0x00. */
    enc_put_bits(e, (uint32_t)ac_codes[0U], ac_sizes[0U]);
  }
}

/* Emit DQT segment for both luma and chroma tables -- see surrounding code and HUM citations. */
static void enc_emit_dqt(ra_jpeg_enc_ctx_t* e)
{
  enc_emit_u16(e, (uint16_t)k_ra_jpeg_marker_dqt);
  enc_emit_u16(e, (uint16_t)(2U + 2U * (1U + (uint16_t)k_ra_jpeg_block_size)));
  enc_emit_u8(e, 0U); /* PqTq=0,0. */
  for (uint8_t i = 0U; i < (uint8_t)k_ra_jpeg_block_size; i++) {
    enc_emit_u8(e, e->qy[s_zigzag[i]]);
  }
  enc_emit_u8(e, 1U); /* PqTq=0,1. */
  for (uint8_t i = 0U; i < (uint8_t)k_ra_jpeg_block_size; i++) {
    enc_emit_u8(e, e->qc[s_zigzag[i]]);
  }
}

/* Emit one DHT segment -- see surrounding code and HUM citations. */
static void enc_emit_dht_one(ra_jpeg_enc_ctx_t* e,
                             uint8_t            tc_th,
                             const uint8_t*     bits,
                             const uint8_t*     vals,
                             uint16_t           total)
{
  enc_emit_u16(e, (uint16_t)k_ra_jpeg_marker_dht);
  enc_emit_u16(e, (uint16_t)(2U + 1U + (uint16_t)k_ra_jpeg_huff_lengths + total));
  enc_emit_u8(e, tc_th);
  for (uint8_t i = 0U; i < (uint8_t)k_ra_jpeg_huff_lengths; i++) {
    enc_emit_u8(e, bits[i]);
  }
  for (uint16_t i = 0U; i < total; i++) {
    enc_emit_u8(e, vals[i]);
  }
}

/* Convert one row of RGB into Y, Cb, Cr samples (BT -- see surrounding code and HUM citations. */
static void enc_rgb_to_ycc_row(const uint8_t* rgb, uint16_t n, int32_t* y, int32_t* cb, int32_t* cr)
{
  for (uint16_t i = 0U; i < n; i++) {
    int32_t r = (int32_t)rgb[i * (uint8_t)k_ra_jpeg_rgb_components + 0U];
    int32_t g = (int32_t)rgb[i * (uint8_t)k_ra_jpeg_rgb_components + 1U];
    int32_t b = (int32_t)rgb[i * (uint8_t)k_ra_jpeg_rgb_components + 2U];
    y[i] =
      (((int32_t)k_ra_jpeg_yr * r) + ((int32_t)k_ra_jpeg_yg * g) + ((int32_t)k_ra_jpeg_yb * b)) >>
      k_ra_jpeg_yuv_shift;
    cb[i] = ((((int32_t)k_ra_jpeg_cbr * r) + ((int32_t)k_ra_jpeg_cbg * g) +
              ((int32_t)k_ra_jpeg_cbb * b)) >>
             k_ra_jpeg_yuv_shift) +
            (int32_t)k_ra_jpeg_level_offset;
    cr[i] = ((((int32_t)k_ra_jpeg_crr * r) + ((int32_t)k_ra_jpeg_crg * g) +
              ((int32_t)k_ra_jpeg_crb * b)) >>
             k_ra_jpeg_yuv_shift) +
            (int32_t)k_ra_jpeg_level_offset;
  }
}

/* Sample one 8x8 luma block from the YCbCr planes -- see surrounding code and HUM citations. */
static void enc_sample_y_block(const int32_t* yplane,
                               uint16_t       plane_w,
                               uint16_t       plane_h,
                               uint16_t       x0,
                               uint16_t       y0,
                               int32_t*       out)
{
  for (uint8_t r = 0U; r < (uint8_t)k_ra_jpeg_block_dim; r++) {
    uint16_t sy = (uint16_t)(y0 + r);
    if (sy >= plane_h) {
      sy = (uint16_t)(plane_h - 1U);
    }
    for (uint8_t c = 0U; c < (uint8_t)k_ra_jpeg_block_dim; c++) {
      uint16_t sx = (uint16_t)(x0 + c);
      if (sx >= plane_w) {
        sx = (uint16_t)(plane_w - 1U);
      }
      out[r * (uint8_t)k_ra_jpeg_block_dim + c] =
        yplane[sy * plane_w + sx] - (int32_t)k_ra_jpeg_level_offset;
    }
  }
}

/* Build one 8x8 chroma block by 2x2 averaging from a 16x16 region -- see surrounding code and HUM citations. */
static void enc_sample_c_block_420(const int32_t* cplane,
                                   uint16_t       plane_w,
                                   uint16_t       plane_h,
                                   uint16_t       x0,
                                   uint16_t       y0,
                                   int32_t*       out)
{
  for (uint8_t r = 0U; r < (uint8_t)k_ra_jpeg_block_dim; r++) {
    for (uint8_t c = 0U; c < (uint8_t)k_ra_jpeg_block_dim; c++) {
      uint16_t sy = (uint16_t)(y0 + r * 2U);
      uint16_t sx = (uint16_t)(x0 + c * 2U);
      int32_t  s  = 0;
      uint8_t  n  = 0U;
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
          s += cplane[yy * plane_w + xx];
          n++;
        }
      }
      out[r * (uint8_t)k_ra_jpeg_block_dim + c] =
        (s / (int32_t)n) - (int32_t)k_ra_jpeg_level_offset;
    }
  }
}

/**
 * @brief Build the four Annex K.3.3 Huffman code/size LUTs in the encoder context.
 *
 * @details
 * Sums each of the four BITS arrays to get the symbol counts, then calls
 * `enc_build_codes()` to materialise the canonical Huffman code values
 * and bit-lengths into the encoder context. The totals are returned via
 * out-params so the caller can pass them to `enc_emit_dht_one()` without
 * recomputing.
 *
 * @param[in,out] e          Encoder context whose code/size LUT arrays are filled.
 * @param[out]    total_dc_l Total luma DC symbols (sum of s_hbits_dc_luma).
 * @param[out]    total_ac_l Total luma AC symbols.
 * @param[out]    total_dc_c Total chroma DC symbols.
 * @param[out]    total_ac_c Total chroma AC symbols.
 *
 * @return None.
 *
 * @pre `e`, `total_dc_l`, `total_ac_l`, `total_dc_c`, `total_ac_c` non-NULL.
 * @pre `e`'s code/size LUT fields are writable.
 * @post All four LUT pairs populated with canonical Huffman codes.
 * @post Totals reflect the static BITS arrays.
 *
 * @note Not thread-safe; caller serializes via encoder context.
 * @since 0.1.0
 */
static void enc_build_huff_tables(ra_jpeg_enc_ctx_t* e,
                                  uint16_t*          total_dc_l,
                                  uint16_t*          total_ac_l,
                                  uint16_t*          total_dc_c,
                                  uint16_t*          total_ac_c)
{
  *total_dc_l = 0U;
  *total_ac_l = 0U;
  *total_dc_c = 0U;
  *total_ac_c = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra_jpeg_huff_lengths; i++) {
    *total_dc_l = (uint16_t)(*total_dc_l + s_hbits_dc_luma[i]);
    *total_ac_l = (uint16_t)(*total_ac_l + s_hbits_ac_luma[i]);
    *total_dc_c = (uint16_t)(*total_dc_c + s_hbits_dc_chroma[i]);
    *total_ac_c = (uint16_t)(*total_ac_c + s_hbits_ac_chroma[i]);
  }
  enc_build_codes(s_hbits_dc_luma, s_hval_dc_luma, e->code_dc_l, e->size_dc_l, *total_dc_l);
  enc_build_codes(s_hbits_ac_luma, s_hval_ac_luma, e->code_ac_l, e->size_ac_l, *total_ac_l);
  enc_build_codes(s_hbits_dc_chroma, s_hval_dc_chroma, e->code_dc_c, e->size_dc_c, *total_dc_c);
  enc_build_codes(s_hbits_ac_chroma, s_hval_ac_chroma, e->code_ac_c, e->size_ac_c, *total_ac_c);
}

/**
 * @brief Emit the SOI marker plus the 16-byte APP0 JFIF header.
 *
 * @details
 * Writes the JFIF 1.1 APP0 segment with `units=0`, aspect ratio 1:1, no
 * embedded thumbnail. Extracted from `enc_emit_headers()` purely to keep
 * the latter under the project's 60-line function-size budget; the byte
 * sequence is identical to what the monolithic implementation produced.
 *
 * @param[in,out] e Encoder context (cursor advances).
 *
 * @return None.
 *
 * @pre `e` non-NULL with a writable `dst` buffer.
 * @pre `e->cap - e->pos >= 22` (SOI + APP0 marker + payload).
 * @post `e->pos` advanced by 22 bytes (SOI + APP0 header + 16-byte payload).
 * @post Byte stream matches the JFIF 1.1 APP0 layout.
 *
 * @note Not thread-safe; caller serializes via encoder context.
 * @since 0.1.0
 */
static void enc_emit_app0_jfif(ra_jpeg_enc_ctx_t* e)
{
  /* SOI. */
  enc_emit_u16(e, (uint16_t)k_ra_jpeg_marker_soi);

  /* APP0 JFIF header (16 bytes payload). */
  enc_emit_u16(e, (uint16_t)k_ra_jpeg_marker_app0);
  enc_emit_u16(e, 16U);
  enc_emit_u8(e, 'J');
  enc_emit_u8(e, 'F');
  enc_emit_u8(e, 'I');
  enc_emit_u8(e, 'F');
  enc_emit_u8(e, 0U);
  enc_emit_u8(e, 1U);
  enc_emit_u8(e, 1U); /* Version 1.1.      */
  enc_emit_u8(e, 0U); /* No density units. */
  enc_emit_u16(e, 1U);
  enc_emit_u16(e, 1U); /* Aspect 1:1. */
  enc_emit_u8(e, 0U);
  enc_emit_u8(e, 0U); /* No thumbnail. */
}

/**
 * @brief Emit the SOI/APP0/DQT/SOF0/DHT/SOS header segments for a 4:2:0 YCbCr stream.
 *
 * @details
 * Writes every byte that precedes the entropy-coded scan, in the order
 * specified by JFIF 1.1: SOI, APP0 (16-byte payload), DQT, SOF0 (3-comp,
 * 4:2:0 sampling, 8-bit), four DHT segments (DC/AC x luma/chroma), and
 * the SOS that hands the four Huffman selectors and the spectral-range
 * bytes to the decoder.
 *
 * @param[in,out] e          Encoder context (cursor advances).
 * @param[in]     w          Image width in pixels.
 * @param[in]     h          Image height in pixels.
 * @param[in]     total_dc_l Symbol count for luma DC HUFFVAL.
 * @param[in]     total_ac_l Symbol count for luma AC HUFFVAL.
 * @param[in]     total_dc_c Symbol count for chroma DC HUFFVAL.
 * @param[in]     total_ac_c Symbol count for chroma AC HUFFVAL.
 *
 * @return None.
 *
 * @pre `e` non-NULL with a writable `dst` buffer.
 * @pre All four totals come from enc_build_huff_tables().
 * @post `e->pos` advanced past every header segment.
 * @post Header bytes byte-identical to the previous monolithic implementation.
 *
 * @note Not thread-safe; caller serializes via encoder context.
 * @since 0.1.0
 */
static void enc_emit_headers(ra_jpeg_enc_ctx_t* e,
                             uint16_t           w,
                             uint16_t           h,
                             uint16_t           total_dc_l,
                             uint16_t           total_ac_l,
                             uint16_t           total_dc_c,
                             uint16_t           total_ac_c)
{
  enc_emit_app0_jfif(e);

  /* DQT, SOF0, DHT x4, SOS. */
  enc_emit_dqt(e);

  /* SOF0: 8-bit, 3 components, 4:2:0. */
  enc_emit_u16(e, (uint16_t)k_ra_jpeg_marker_sof0);
  enc_emit_u16(e, k_jpeg_sof_seg_len);
  enc_emit_u8(e, 8U);
  enc_emit_u16(e, h);
  enc_emit_u16(e, w);
  enc_emit_u8(e, 3U);
  /* Y: id 1, 2x2 sampling, qtab 0. */
  enc_emit_u8(e, 1U);
  enc_emit_u8(e, k_jpeg_samp_2x2);
  enc_emit_u8(e, 0U);
  /* Cb: id 2, 1x1, qtab 1. */
  enc_emit_u8(e, 2U);
  enc_emit_u8(e, k_jpeg_samp_1x1);
  enc_emit_u8(e, 1U);
  /* Cr: id 3, 1x1, qtab 1. */
  enc_emit_u8(e, 3U);
  enc_emit_u8(e, k_jpeg_samp_1x1);
  enc_emit_u8(e, 1U);

  enc_emit_dht_one(e, 0x00U, s_hbits_dc_luma, s_hval_dc_luma, total_dc_l);
  enc_emit_dht_one(e, 0x10U, s_hbits_ac_luma, s_hval_ac_luma, total_ac_l);
  enc_emit_dht_one(e, 0x01U, s_hbits_dc_chroma, s_hval_dc_chroma, total_dc_c);
  enc_emit_dht_one(e, k_jpeg_dht_ac_chroma, s_hbits_ac_chroma, s_hval_ac_chroma, total_ac_c);

  /* SOS. */
  enc_emit_u16(e, (uint16_t)k_ra_jpeg_marker_sos);
  enc_emit_u16(e, k_jpeg_sos_seg_len);
  enc_emit_u8(e, 3U);
  enc_emit_u8(e, 1U);
  enc_emit_u8(e, 0x00U); /* Y: dc=0,ac=0. */
  enc_emit_u8(e, 2U);
  enc_emit_u8(e, k_jpeg_sos_sel_chroma); /* Cb. */
  enc_emit_u8(e, 3U);
  enc_emit_u8(e, k_jpeg_sos_sel_chroma); /* Cr. */
  enc_emit_u8(e, 0U);
  enc_emit_u8(e, k_jpeg_spectral_end);
  enc_emit_u8(e, 0U);
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
 * @return None.
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
        ((uint32_t)src_y * (uint32_t)w + (uint32_t)src_x) * (uint32_t)k_ra_jpeg_rgb_components;
      tmp_rgb[c * (uint8_t)k_ra_jpeg_rgb_components + 0U] = rgb[sidx + 0U];
      tmp_rgb[c * (uint8_t)k_ra_jpeg_rgb_components + 1U] = rgb[sidx + 1U];
      tmp_rgb[c * (uint8_t)k_ra_jpeg_rgb_components + 2U] = rgb[sidx + 2U];
    }
    enc_rgb_to_ycc_row(tmp_rgb,
                       pad_w,
                       &y_strip[r * pad_w],
                       &cb_strip[r * pad_w],
                       &cr_strip[r * pad_w]);
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
 * @return None.
 *
 * @pre `e` is initialized with valid Huffman LUTs.
 * @pre `pad_w > 0` and a multiple of 16.
 * @post `e->pos`/bit buffer advanced by the entropy bytes of every MCU in the strip.
 * @post Block sample order is byte-identical to the previous monolithic encoder.
 *
 * @note Not thread-safe; caller serializes via encoder context.
 * @since 0.1.0
 */
static void enc_encode_mcu_row(ra_jpeg_enc_ctx_t* e,
                               uint16_t           pad_w,
                               const int32_t*     y_strip,
                               const int32_t*     cb_strip,
                               const int32_t*     cr_strip)
{
  for (uint16_t mbx = 0U; mbx < pad_w; mbx = (uint16_t)(mbx + 16U)) {
    int32_t blk[(uint32_t)k_ra_jpeg_block_size];
    /* 4 luma blocks: (0,0) (0,8) (8,0) (8,8). */
    for (uint8_t by = 0U; by < 2U; by++) {
      for (uint8_t bx = 0U; bx < 2U; bx++) {
        enc_sample_y_block(y_strip,
                           pad_w,
                           16U,
                           (uint16_t)(mbx + bx * 8U),
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

/* Top-level encode driver -- emits the full JFIF byte stream -- see surrounding code and HUM citations. */
static ra_err_t
enc_run(ra_jpeg_enc_ctx_t* e, const uint8_t* rgb, uint16_t w, uint16_t h, uint8_t quality)
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
  enc_build_huff_tables(e, &total_dc_l, &total_ac_l, &total_dc_c, &total_ac_c);

  enc_emit_headers(e, w, h, total_dc_l, total_ac_l, total_dc_c, total_ac_c);

  /* Convert source rows to YCbCr 16-row strips. Static buffers
   * keep the host stack small while still respecting NASA Rule 3
   * (no heap). The encoder is single-threaded, so file-scope
   * sharing is safe. */
  uint16_t pad_w = (uint16_t)((w + k_jpeg_mcu_align) & ~k_jpeg_mcu_align);
  uint16_t pad_h = (uint16_t)((h + k_jpeg_mcu_align) & ~k_jpeg_mcu_align);
  if ((uint32_t)pad_w > (uint32_t)k_ra_jpeg_enc_max_w) {
    return k_ra_err_invalid_arg;
  }
  static int32_t s_y_strip[16U * (uint32_t)k_ra_jpeg_enc_max_w];
  static int32_t s_cb_strip[16U * (uint32_t)k_ra_jpeg_enc_max_w];
  static int32_t s_cr_strip[16U * (uint32_t)k_ra_jpeg_enc_max_w];
  static uint8_t s_tmp_rgb[3U * (uint32_t)k_ra_jpeg_enc_max_w];

  for (uint16_t mby = 0U; mby < pad_h; mby = (uint16_t)(mby + 16U)) {
    enc_convert_strip_to_ycc(rgb, w, h, pad_w, mby, s_y_strip, s_cb_strip, s_cr_strip, s_tmp_rgb);
    enc_encode_mcu_row(e, pad_w, s_y_strip, s_cb_strip, s_cr_strip);
  }

  enc_flush_bits(e);
  enc_emit_u16(e, (uint16_t)k_ra_jpeg_marker_eoi);

  if (e->overflow) {
    return k_ra_err_invalid_size;
  }
  return k_ra_ok;
}

ra_err_t ra_jpeg_sw_encode(const uint8_t* rgb_buf,
                           uint16_t       width,
                           uint16_t       height,
                           uint8_t        quality,
                           uint8_t*       out_buf,
                           uint32_t       out_buf_len,
                           uint32_t*      out_len)
{
  RA_CHECK_NULL_PTR(rgb_buf, s_tag, "rgb_buf is NULL");
  RA_CHECK_NULL_PTR(out_buf, s_tag, "out_buf is NULL");
  RA_CHECK_NULL_PTR(out_len, s_tag, "out_len is NULL");
  *out_len = 0U;
  if (width == 0U || height == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (quality < (uint8_t)k_ra_jpeg_sw_quality_min || quality > (uint8_t)k_ra_jpeg_sw_quality_max) {
    return k_ra_err_invalid_arg;
  }

  /* Encoder context contains 2KiB of Huffman code/size LUTs; allocate
   * static to avoid the project's stack-usage budget firing. */
  static ra_jpeg_enc_ctx_t s_e;
  ra_jpeg_enc_ctx_t*       e = &s_e;
  memset(e, 0, sizeof(*e));
  e->dst = out_buf;
  e->cap = out_buf_len;

  ra_err_t err = enc_run(e, rgb_buf, width, height, quality);
  if (err != k_ra_ok) {
    return err;
  }
  *out_len = e->pos;
  return k_ra_ok;
}

// NOLINTEND(readability-function-size,readability-function-cognitive-complexity,readability-redundant-casting,readability-math-missing-parentheses,bugprone-implicit-widening-of-multiplication-result,clang-analyzer-core.DivideZero,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
