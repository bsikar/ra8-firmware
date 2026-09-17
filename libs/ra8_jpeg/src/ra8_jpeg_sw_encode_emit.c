/**
 * @file ra8_jpeg_sw_encode_emit.c
 * @brief Software JPEG encoder: byte/bit emitters, Annex K.3.3 tables
 *        and JFIF header-segment writers.
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * Emitter half of the software JPEG encoder: the bounds-checked byte
 * emitter, the MSB-first bit packer with T.81 sec F.1.2.3 stuffing,
 * the canonical Huffman code build over the T.81 Annex K.3.3
 * reference tables, and the SOI/APP0/DQT/SOF0/DHT/SOS header-segment
 * writers. The pixel pipeline (RGB->YCbCr conversion, sampling,
 * forward DCT, quantization, per-block emission) and the
 * `ra8_jpeg_sw_encode()` public API live in `ra8_jpeg_sw_encode.c`;
 * the symbols shared between the two units are declared in
 * `ra8_jpeg_sw_encode_internal.h`.
 *
 * The Huffman BITS/HUFFVAL tables in this file are taken verbatim
 * from ITU-T T.81 / ISO 10918-1 Annex K.3.3.
 *
 * Spec citations are tagged `T.81 sec X.Y "..."` and refer to
 * ITU-T Recommendation T.81 (1992) | ISO/IEC 10918-1.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_jpeg_sw.h"
#include "ra8_jpeg_sw_encode_internal.h"
#include "ra8_jpeg_sw_internal.h"

/* ------------------------------------------------------------------ */
/* Annex K.3.3 reference Huffman tables (luma DC/AC, chroma DC/AC) */
/* ------------------------------------------------------------------ */

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
/* Byte / bit emitters */
/* ------------------------------------------------------------------ */

/**
 * @brief Append one byte; sets `overflow` on capacity exhaustion.
 *
 * @details
 * Single choke point for every byte that reaches the output buffer:
 * once `e->pos` hits `e->cap` the byte is dropped and `e->overflow`
 * latches so the driver can fail the encode with
 * `k_ra8_err_invalid_size` after the fact.
 *
 * @param[in,out] e Encoder context (cursor advances).
 * @param[in]     b Byte to append.
 *
 * @pre `e` is non-NULL (module-internal call chain).
 * @pre `e->dst` holds `e->cap` writable bytes.
 * @post `e->pos` advanced by 1, or `e->overflow` set.
 * @post No write ever lands at or past `e->cap`.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_enc_emit_u8(ra8_jpeg_enc_ctx_t* e, uint8_t b)
{
  if (e->pos >= e->cap) {
    e->overflow = true;
    return;
  }
  e->dst[e->pos] = b;
  e->pos++;
}

/** @brief Implementation of `priv_jpeg_sw_enc_emit_u16()` -- big-endian byte pair. */
RA8_PRIV void priv_jpeg_sw_enc_emit_u16(ra8_jpeg_enc_ctx_t* e, uint16_t v)
{
  internal_enc_emit_u8(e, (uint8_t)(v >> k_ra8_jpeg_byte_shift));
  internal_enc_emit_u8(e, (uint8_t)(v & k_jpeg_byte_mask));
}

/** @brief Implementation of `priv_jpeg_sw_enc_put_bits()` -- T.81 F.1.2.3 stuffing. */
RA8_PRIV void priv_jpeg_sw_enc_put_bits(ra8_jpeg_enc_ctx_t* e, uint32_t code, uint8_t n)
{
  e->bit_buf = (e->bit_buf << n) | (code & ((1U << n) - 1U));
  e->bit_cnt = (uint8_t)(e->bit_cnt + n);
  while (e->bit_cnt >= (uint8_t)k_ra8_jpeg_byte_shift) {
    uint8_t b = (uint8_t)(e->bit_buf >> (e->bit_cnt - k_ra8_jpeg_byte_shift));
    internal_enc_emit_u8(e, b);
    if (b == (uint8_t)k_ra8_jpeg_marker_byte) {
      internal_enc_emit_u8(e, 0U);
    }
    e->bit_cnt = (uint8_t)(e->bit_cnt - k_ra8_jpeg_byte_shift);
  }
}

/** @brief Implementation of `priv_jpeg_sw_enc_flush_bits()` -- 1-fill pad. */
RA8_PRIV void priv_jpeg_sw_enc_flush_bits(ra8_jpeg_enc_ctx_t* e)
{
  if (e->bit_cnt > 0U) {
    uint32_t pad = (1U << (k_ra8_jpeg_byte_shift - e->bit_cnt)) - 1U;
    priv_jpeg_sw_enc_put_bits(e,
                              ((e->bit_buf << (k_ra8_jpeg_byte_shift - e->bit_cnt)) | pad) &
                                k_jpeg_byte_mask,
                              (uint8_t)(k_ra8_jpeg_byte_shift - e->bit_cnt));
  }
}

/* ------------------------------------------------------------------ */
/* Canonical Huffman code build (T.81 Annex C) */
/* ------------------------------------------------------------------ */

/**
 * @brief Build the canonical-code table from BITS+HUFFVAL.
 *
 * @details
 * T.81 Annex C "Generation of size table" + "Generation of code
 * table": expands the BITS list into per-symbol code lengths, assigns
 * consecutive canonical codes within each length, and scatters both
 * into symbol-indexed LUTs so the block encoder can look codes up by
 * symbol value directly.
 *
 * @param[in]  bits  16-entry BITS list (codes per length 1..16).
 * @param[in]  vals  HUFFVAL symbol list (`total` entries).
 * @param[out] codes 256-entry code LUT indexed by symbol value.
 * @param[out] sizes 256-entry code-length LUT indexed by symbol value.
 * @param[in]  total Number of symbols in `vals`.
 *
 * @pre `bits`/`vals` come from the static Annex K.3.3 tables.
 * @pre `total` equals the sum of `bits[0..15]` (<= 256).
 * @post `codes[v]`/`sizes[v]` are valid for every symbol `v` in `vals`.
 * @post LUT entries for absent symbols are untouched.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_enc_build_codes(const uint8_t* bits,
                                                  const uint8_t* vals,
                                                  uint16_t*      codes,
                                                  uint8_t*       sizes,
                                                  uint16_t       total)
{
  uint8_t  huffsize[k_ra8_jpeg_huff_max + 1U];
  uint16_t k = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_jpeg_huff_lengths; i++) {
    for (uint8_t j = 0U; j < bits[i]; j++) {
      huffsize[k] = (uint8_t)(i + 1U);
      k++;
    }
  }
  huffsize[k] = 0U;

  uint16_t huffcode[k_ra8_jpeg_huff_max + 1U];
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

/** @brief Implementation of `priv_jpeg_sw_enc_build_huff()` -- four K.3.3 LUT builds. */
RA8_PRIV void priv_jpeg_sw_enc_build_huff(ra8_jpeg_enc_ctx_t* e,
                                          uint16_t*           total_dc_l,
                                          uint16_t*           total_ac_l,
                                          uint16_t*           total_dc_c,
                                          uint16_t*           total_ac_c)
{
  *total_dc_l = 0U;
  *total_ac_l = 0U;
  *total_dc_c = 0U;
  *total_ac_c = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_jpeg_huff_lengths; i++) {
    *total_dc_l = (uint16_t)(*total_dc_l + s_hbits_dc_luma[i]);
    *total_ac_l = (uint16_t)(*total_ac_l + s_hbits_ac_luma[i]);
    *total_dc_c = (uint16_t)(*total_dc_c + s_hbits_dc_chroma[i]);
    *total_ac_c = (uint16_t)(*total_ac_c + s_hbits_ac_chroma[i]);
  }
  internal_enc_build_codes(s_hbits_dc_luma,
                           s_hval_dc_luma,
                           e->code_dc_l,
                           e->size_dc_l,
                           *total_dc_l);
  internal_enc_build_codes(s_hbits_ac_luma,
                           s_hval_ac_luma,
                           e->code_ac_l,
                           e->size_ac_l,
                           *total_ac_l);
  internal_enc_build_codes(s_hbits_dc_chroma,
                           s_hval_dc_chroma,
                           e->code_dc_c,
                           e->size_dc_c,
                           *total_dc_c);
  internal_enc_build_codes(s_hbits_ac_chroma,
                           s_hval_ac_chroma,
                           e->code_ac_c,
                           e->size_ac_c,
                           *total_ac_c);
}

/* ------------------------------------------------------------------ */
/* JFIF header segments */
/* ------------------------------------------------------------------ */

/**
 * @brief Emit the DQT segment for both luma and chroma tables.
 *
 * @details One DQT marker carrying two 8-bit-precision tables (PqTq
 *          0,0 then 0,1), each serialised in zig-zag order per T.81
 *          sec B.2.4.1.
 *
 * @param[in,out] e Encoder context (cursor advances).
 *
 * @pre `e->qy`/`e->qc` hold the scaled quantization tables.
 * @pre `e` is non-NULL (module-internal call chain).
 * @post Both tables emitted inside a single DQT segment.
 * @post `e->pos` advanced by the full segment length.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_enc_emit_dqt(ra8_jpeg_enc_ctx_t* e)
{
  priv_jpeg_sw_enc_emit_u16(e, (uint16_t)k_ra8_jpeg_marker_dqt);
  priv_jpeg_sw_enc_emit_u16(e, (uint16_t)(2U + (2U * (1U + (uint16_t)k_ra8_jpeg_block_size))));
  internal_enc_emit_u8(e, 0U); /* PqTq=0,0. */
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_jpeg_block_size; i++) {
    internal_enc_emit_u8(e, e->qy[s_zigzag[i]]);
  }
  internal_enc_emit_u8(e, 1U); /* PqTq=0,1. */
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_jpeg_block_size; i++) {
    internal_enc_emit_u8(e, e->qc[s_zigzag[i]]);
  }
}

/**
 * @brief Emit one DHT segment (T.81 sec B.2.4.2).
 *
 * @details Serialises the TcTh selector, the 16-entry BITS list and
 *          the HUFFVAL symbol list of one reference Huffman table.
 *
 * @param[in,out] e     Encoder context (cursor advances).
 * @param[in]     tc_th Table class/id byte (0x00/0x10/0x01/0x11).
 * @param[in]     bits  16-entry BITS list.
 * @param[in]     vals  HUFFVAL list (`total` entries).
 * @param[in]     total Symbol count for the segment length field.
 *
 * @pre `bits`/`vals` are the static Annex K.3.3 tables.
 * @pre `total` matches the sum of `bits[0..15]`.
 * @post One complete DHT segment appended to the stream.
 * @post `e->pos` advanced by the full segment length.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_enc_emit_dht_one(ra8_jpeg_enc_ctx_t* e,
                                                   uint8_t             tc_th,
                                                   const uint8_t*      bits,
                                                   const uint8_t*      vals,
                                                   uint16_t            total)
{
  priv_jpeg_sw_enc_emit_u16(e, (uint16_t)k_ra8_jpeg_marker_dht);
  priv_jpeg_sw_enc_emit_u16(e, (uint16_t)(2U + 1U + (uint16_t)k_ra8_jpeg_huff_lengths + total));
  internal_enc_emit_u8(e, tc_th);
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_jpeg_huff_lengths; i++) {
    internal_enc_emit_u8(e, bits[i]);
  }
  for (uint16_t i = 0U; i < total; i++) {
    internal_enc_emit_u8(e, vals[i]);
  }
}

/**
 * @brief Emit the SOI marker plus the 16-byte APP0 JFIF header.
 *
 * @details
 * Writes the JFIF 1.1 APP0 segment with `units=0`, aspect ratio 1:1,
 * no embedded thumbnail. The byte sequence is identical to what the
 * pre-split monolithic encoder produced.
 *
 * @param[in,out] e Encoder context (cursor advances).
 *
 * @pre `e` is non-NULL with a writable `dst` buffer.
 * @pre `e->pos` is 0 (SOI opens the stream).
 * @post `e->pos` advanced by 22 bytes (SOI + APP0 header + payload).
 * @post Byte stream matches the JFIF 1.1 APP0 layout.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_enc_emit_app0_jfif(ra8_jpeg_enc_ctx_t* e)
{
  /* SOI. */
  priv_jpeg_sw_enc_emit_u16(e, (uint16_t)k_ra8_jpeg_marker_soi);

  /* APP0 JFIF header (16 bytes payload). */
  priv_jpeg_sw_enc_emit_u16(e, (uint16_t)k_ra8_jpeg_marker_app0);
  priv_jpeg_sw_enc_emit_u16(e, 16U);
  internal_enc_emit_u8(e, 'J');
  internal_enc_emit_u8(e, 'F');
  internal_enc_emit_u8(e, 'I');
  internal_enc_emit_u8(e, 'F');
  internal_enc_emit_u8(e, 0U);
  internal_enc_emit_u8(e, 1U);
  internal_enc_emit_u8(e, 1U); /* Version 1.1.      */
  internal_enc_emit_u8(e, 0U); /* No density units. */
  priv_jpeg_sw_enc_emit_u16(e, 1U);
  priv_jpeg_sw_enc_emit_u16(e, 1U); /* Aspect 1:1. */
  internal_enc_emit_u8(e, 0U);
  internal_enc_emit_u8(e, 0U); /* No thumbnail. */
}

/**
 * @brief Emit the SOF0 frame header for a 3-component 4:2:0 stream.
 *
 * @details T.81 sec B.2.2: 8-bit precision, the image dimensions, and
 *          the three component records (Y 2x2 sampling on quant table
 *          0; Cb/Cr 1x1 on quant table 1).
 *
 * @param[in,out] e Encoder context (cursor advances).
 * @param[in]     w Image width in pixels.
 * @param[in]     h Image height in pixels.
 *
 * @pre `e` is non-NULL (module-internal call chain).
 * @pre `w` and `h` are non-zero (public API validated them).
 * @post One complete SOF0 segment appended to the stream.
 * @post `e->pos` advanced by the full segment length.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_enc_emit_sof0(ra8_jpeg_enc_ctx_t* e, uint16_t w, uint16_t h)
{
  priv_jpeg_sw_enc_emit_u16(e, (uint16_t)k_ra8_jpeg_marker_sof0);
  priv_jpeg_sw_enc_emit_u16(e, k_jpeg_sof_seg_len);
  internal_enc_emit_u8(e, 8U);
  priv_jpeg_sw_enc_emit_u16(e, h);
  priv_jpeg_sw_enc_emit_u16(e, w);
  internal_enc_emit_u8(e, 3U);
  /* Y: id 1, 2x2 sampling, qtab 0. */
  internal_enc_emit_u8(e, 1U);
  internal_enc_emit_u8(e, k_jpeg_samp_2x2);
  internal_enc_emit_u8(e, 0U);
  /* Cb: id 2, 1x1, qtab 1. */
  internal_enc_emit_u8(e, 2U);
  internal_enc_emit_u8(e, k_jpeg_samp_1x1);
  internal_enc_emit_u8(e, 1U);
  /* Cr: id 3, 1x1, qtab 1. */
  internal_enc_emit_u8(e, 3U);
  internal_enc_emit_u8(e, k_jpeg_samp_1x1);
  internal_enc_emit_u8(e, 1U);
}

/**
 * @brief Emit the SOS scan header (T.81 sec B.2.3).
 *
 * @details Binds the three scan components to their Huffman selectors
 *          (Y dc=0/ac=0, Cb/Cr dc=1/ac=1) and writes the baseline
 *          spectral-range bytes (Ss=0, Se=63, AhAl=0).
 *
 * @param[in,out] e Encoder context (cursor advances).
 *
 * @pre `e` is non-NULL (module-internal call chain).
 * @pre The four DHT segments were already emitted.
 * @post One complete SOS segment appended to the stream.
 * @post The stream is positioned for entropy-coded data.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_enc_emit_sos(ra8_jpeg_enc_ctx_t* e)
{
  priv_jpeg_sw_enc_emit_u16(e, (uint16_t)k_ra8_jpeg_marker_sos);
  priv_jpeg_sw_enc_emit_u16(e, k_jpeg_sos_seg_len);
  internal_enc_emit_u8(e, 3U);
  internal_enc_emit_u8(e, 1U);
  internal_enc_emit_u8(e, 0x00U); /* Y: dc=0,ac=0. */
  internal_enc_emit_u8(e, 2U);
  internal_enc_emit_u8(e, k_jpeg_sos_sel_chroma); /* Cb. */
  internal_enc_emit_u8(e, 3U);
  internal_enc_emit_u8(e, k_jpeg_sos_sel_chroma); /* Cr. */
  internal_enc_emit_u8(e, 0U);
  internal_enc_emit_u8(e, k_jpeg_spectral_end);
  internal_enc_emit_u8(e, 0U);
}

/** @brief Implementation of `priv_jpeg_sw_enc_headers()` -- JFIF 1.1 segment order. */
RA8_PRIV void priv_jpeg_sw_enc_headers(ra8_jpeg_enc_ctx_t* e,
                                       uint16_t            w,
                                       uint16_t            h,
                                       uint16_t            total_dc_l,
                                       uint16_t            total_ac_l,
                                       uint16_t            total_dc_c,
                                       uint16_t            total_ac_c)
{
  internal_enc_emit_app0_jfif(e);
  internal_enc_emit_dqt(e);
  internal_enc_emit_sof0(e, w, h);

  internal_enc_emit_dht_one(e, 0x00U, s_hbits_dc_luma, s_hval_dc_luma, total_dc_l);
  internal_enc_emit_dht_one(e, 0x10U, s_hbits_ac_luma, s_hval_ac_luma, total_ac_l);
  internal_enc_emit_dht_one(e, 0x01U, s_hbits_dc_chroma, s_hval_dc_chroma, total_dc_c);
  internal_enc_emit_dht_one(e,
                            k_jpeg_dht_ac_chroma,
                            s_hbits_ac_chroma,
                            s_hval_ac_chroma,
                            total_ac_c);

  internal_enc_emit_sos(e);
}
