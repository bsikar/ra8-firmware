/**
 * @file ra8_jpeg_sw_internal.h
 * @brief Module-private declarations shared across the software JPEG
 * @ingroup grp_ereader
 *        codec translation units.
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * The pure-software baseline JPEG codec was C across three translation
 * units, which shared this header. It is Zig throughout now: the
 * encoder, the decoder, and no `.c` is left in
 * this library.
 *
 * What remains here is the surface one out-of-tree C suite still drives
 * directly, `tests/graphics/src/test_ra8_jpeg_sw_cov.c`, which builds a
 * bit reader and a Huffman table on its own stack and exercises the
 * three entropy primitives against them. So this header keeps those two
 * struct types, whose layout the Zig side asserts against C at compile
 * time, the constant blocks and DSP tables those declarations sit among,
 * and exactly three prototypes. Everything else the decoder needs is
 * internal to the Zig implementation and no longer declared.
 *
 * Spec citations are tagged `T.81 sec X.Y "..."` and refer to
 * ITU-T Recommendation T.81 (1992) | ISO/IEC 10918-1.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"

/* ------------------------------------------------------------------ */
/* Shared constants and look-up tables */
/* ------------------------------------------------------------------ */

/**
 * @enum jpeg_misc_t
 * @brief Misc JPEG masks / offsets.
 */
typedef enum : uint16_t {
  k_jpeg_byte_mask    = 0xFFU, /**< JPEG byte mask.    */
  k_jpeg_nibble_mask  = 0xFU,  /**< JPEG nibble mask.  */
  k_jpeg_reservoir_lo = 24U,   /**< JPEG reservoir lo. */
  k_jpeg_sof_dims_off = 5U,    /**< JPEG sof dims off. */
  k_jpeg_mcu_align    = 15U,   /**< JPEG mcu align.    */
  k_jpeg_zrl_symbol   = 0xF0U, /**< JPEG zrl symbol.   */
} jpeg_misc_t;

/** @brief Fixed-point IDCT / YCbCr shift amounts. */
typedef enum : uint8_t {
  k_jpeg_idct_p1_bias_sh = 27U, /**< JPEG idct p1 bias sh. */
  k_jpeg_idct_p1_sh      = 28U, /**< JPEG idct p1 sh.      */
  k_jpeg_idct_p2_bias_sh = 13U, /**< JPEG idct p2 bias sh. */
  k_jpeg_q14_shift       = 14U, /**< JPEG q14 shift.       */
  k_jpeg_ycc_q15_shift   = 14U, /**< JPEG ycc q15 shift.   */
} jpeg_dsp_shift_t;

/** @brief Q15 BT.601 YCbCr->RGB coefficients (== Q16 / 2). */
typedef enum : int16_t {
  k_cr_r_q15 = 22970,  /**< 1.40200 / 2 * 2^15. */
  k_cb_b_q15 = 29032,  /**< 1.77200 / 2 * 2^15. */
  k_cb_g_q15 = -5638,  /**< Cb g q15.           */
  k_cr_g_q15 = -11700, /**< Cr g q15.           */
} jpeg_ycc_coeff_t;

/** @brief JPEG SOF marker range and stuffing markers. */
typedef enum : uint16_t {
  k_jpeg_marker_sof_lo = 0xFFC0U, /**< JPEG marker sof lo. */
  k_jpeg_marker_sof1   = 0xFFC1U, /**< JPEG marker sof1.   */
  k_jpeg_marker_sof_hi = 0xFFCFU, /**< JPEG marker sof hi. */
  k_jpeg_marker_jpg    = 0xFFC8U, /**< JPEG marker jpg.    */
  k_jpeg_marker_ff00   = 0xFF00U, /**< JPEG marker ff00.   */
} jpeg_marker_range_t;

/** @brief JPEG encoder quality scaling and segment field values. */
typedef enum : uint16_t {
  k_jpeg_q_scale_low  = 5000U, /**< JPEG q scale low.    */
  k_jpeg_q_scale_high = 200U,  /**< JPEG q scale high.   */
  k_jpeg_q_round_bias = 50U,   /**< JPEG q round bias.   */
  k_jpeg_q_percent    = 100U,  /**< JPEG q percent.      */
  k_jpeg_sof_seg_len  = 17U,   /**< JPEG sof seg length. */
  k_jpeg_sos_seg_len  = 12U,   /**< JPEG sos seg length. */
  k_jpeg_spectral_end = 63U,   /**< JPEG spectral end.   */
} jpeg_enc_t;

/** @brief JPEG component sampling / table-selector bytes (positional). */
typedef enum : uint8_t {
  k_jpeg_samp_2x2       = 0x22U, /**< JPEG samp 2x2.       */
  k_jpeg_samp_1x1       = 0x11U, /**< JPEG samp 1x1.       */
  k_jpeg_dht_ac_chroma  = 0x11U, /**< JPEG dht ac chroma.  */
  k_jpeg_sos_sel_chroma = 0x11U, /**< JPEG sos sel chroma. */
} jpeg_enc_field_t;

/**
 * @enum ra8_jpeg_marker_t
 * @brief JPEG segment marker bytes (T.81 sec B.1.1.3 "Marker
 *        assignments"). The high byte is always 0xFF.
 */
typedef enum : uint16_t {
  k_ra8_jpeg_marker_soi    = 0xFFD8U,              /**< Start of image.            */
  k_ra8_jpeg_marker_eoi    = 0xFFD9U,              /**< End of image.              */
  k_ra8_jpeg_marker_sos    = 0xFFDAU,              /**< Start of scan.             */
  k_ra8_jpeg_marker_dqt    = 0xFFDBU,              /**< Define quantization table. */
  k_ra8_jpeg_marker_dht    = 0xFFC4U,              /**< Define Huffman table.      */
  k_ra8_jpeg_marker_dri    = 0xFFDDU,              /**< Define restart interval.   */
  k_ra8_jpeg_marker_sof0   = 0xFFC0U,              /**< Baseline DCT, Huffman.     */
  k_ra8_jpeg_marker_sof1   = 0xFFC1U,              /**< Extended sequential DCT.   */
  k_ra8_jpeg_marker_sof2   = 0xFFC2U,              /**< Progressive DCT.           */
  k_ra8_jpeg_marker_sof3   = 0xFFC3U,              /**< Lossless.                  */
  k_ra8_jpeg_marker_app0   = 0xFFE0U,              /**< JFIF APP0.                 */
  k_ra8_jpeg_marker_com    = 0xFFFEU,              /**< Comment.                   */
  k_ra8_jpeg_marker_rst0   = 0xFFD0U,              /**< Restart 0.                 */
  k_ra8_jpeg_marker_rst7   = 0xFFD7U,              /**< Restart 7.                 */
  k_ra8_jpeg_marker_pad    = 0xFF00U,              /**< Stuffed byte (data 0xFF).  */
  k_ra8_jpeg_marker_sof_lo = 0xFFC0U,              /**< First SOF marker code.     */
  k_ra8_jpeg_marker_sof_hi = k_jpeg_marker_sof_hi, /**< Last SOF marker code.      */
  k_ra8_jpeg_marker_dac    = 0xFFC8U,              /**< DAC marker (skipped).      */
  k_ra8_jpeg_marker_high   = 0xFF00U,              /**< Marker high-byte mask.     */
} ra8_jpeg_marker_t;

/**
 * @enum ra8_jpeg_const_t
 * @brief Sizes and indices used throughout the codec.
 */
typedef enum : uint16_t {
  k_ra8_jpeg_block_dim    = 8U,    /**< 8x8 DCT block edge.            */
  k_ra8_jpeg_block_size   = 64U,   /**< Coefficients per block.        */
  k_ra8_jpeg_max_comps    = 3U,    /**< YCbCr or grayscale only.       */
  k_ra8_jpeg_mcu_max_dim  = 16U,   /**< 4:2:0 MCU is 16x16 luma px.    */
  k_ra8_jpeg_huff_classes = 2U,    /**< DC + AC.                       */
  k_ra8_jpeg_huff_ids     = 2U,    /**< Luma + chroma table id.        */
  k_ra8_jpeg_huff_max     = 256U,  /**< Max symbols per Huffman table. */
  k_ra8_jpeg_marker_byte  = 0xFFU, /**< RA8 JPEG marker byte.          */
  k_ra8_jpeg_quant_tabs   = 2U,    /**< One luma + one chroma table.   */
  k_ra8_jpeg_enc_max_w    = 1024U, /**< Encoder max image width (px).  */
  k_ra8_jpeg_min_jpeg_len = 4U,    /**< Smallest plausible stream.     */
  k_ra8_jpeg_garbage_len  = 5U,    /**< Garbage-input cutoff.          */
  k_ra8_jpeg_mcu_align    = 15U,   /**< MCU alignment mask.            */
  k_ra8_jpeg_sof0_min_len = 8U,    /**< Min SOF0 segment length.       */
  k_ra8_jpeg_dht_hdr      = 1U,    /**< DHT TcTh byte size.            */
  k_ra8_jpeg_sof0_hdr_len = 17U,   /**< SOF0 segment length we emit.   */
  k_ra8_jpeg_sos_len      = 12U,   /**< SOS segment length we emit.    */
  k_ra8_jpeg_app0_len     = 16U,   /**< APP0 (JFIF) segment length.    */
  k_ra8_jpeg_eob_band_max = 63U,   /**< Last AC zig-zag index.         */
} ra8_jpeg_const_t;

/**
 * @enum ra8_jpeg_shift_t
 * @brief Shift amounts and signed-cast helpers.
 */
typedef enum : uint8_t {
  k_ra8_jpeg_byte_shift     = 8U,    /**< RA8 JPEG byte shift.          */
  k_ra8_jpeg_nibble_shift   = 4U,    /**< RA8 JPEG nibble shift.        */
  k_ra8_jpeg_nibble_mask    = 0x0FU, /**< RA8 JPEG nibble mask.         */
  k_ra8_jpeg_signbit_pos    = 7U,    /**< RA8 JPEG signbit pos.         */
  k_ra8_jpeg_signbit_pos16  = 15U,   /**< RA8 JPEG signbit pos16.       */
  k_ra8_jpeg_level_offset   = 128U,  /**< RA8 JPEG level offset.        */
  k_ra8_jpeg_pixel_max      = 255U,  /**< RA8 JPEG pixel maximum.       */
  k_ra8_jpeg_quality_pivot  = 50U,   /**< RA8 JPEG quality pivot.       */
  k_ra8_jpeg_huff_lengths   = 16U,   /**< 16 BITS-list slots.           */
  k_ra8_jpeg_zrl_runlen     = 16U,   /**< Zero-Run-Length symbol skip.  */
  k_ra8_jpeg_zrl_symbol     = 0xF0U, /**< T.81 K.3.3 ZRL byte.          */
  k_ra8_jpeg_eob_runlen     = 0xFU,  /**< F0 high-nibble RRRR mask.     */
  k_ra8_jpeg_dht_dc_class   = 0U,    /**< RA8 JPEG dht dc class.        */
  k_ra8_jpeg_dht_ac_class   = 0x10U, /**< RA8 JPEG dht ac class.        */
  k_ra8_jpeg_y_sampling_420 = 0x22U, /**< SOF0 H/V byte for Y in 4:2:0. */
  k_ra8_jpeg_c_sampling     = 0x11U, /**< SOF0 H/V byte for chroma.     */
  k_ra8_jpeg_be_byte_mask   = 0xFFU, /**< Low-byte mask.                */
  k_ra8_jpeg_yuv_shift      = 16U,   /**< BT.601 fixed-point shift.     */
  k_ra8_jpeg_rgb_components = 3U,    /**< RA8 JPEG RGB components.      */
} ra8_jpeg_shift_t;

/**
 * @enum ra8_jpeg_color_coef_t
 * @brief BT.601 RGB<->YCbCr fixed-point coefficients (Q16).
 *
 * @details
 * Same constants the IJG `jccolor.c` and `jdcolor.c` files use
 * (see also T.871 / JFIF colour conversion). All values are
 * fixed-point Q16 (multiply by `1 << 16` and round).
 */
typedef enum : int32_t {
  k_ra8_jpeg_yr   = 19595,  /**< 0.29900 * 65536. */
  k_ra8_jpeg_yg   = 38470,  /**< 0.58700 * 65536. */
  k_ra8_jpeg_yb   = 7471,   /**< 0.11400 * 65536. */
  k_ra8_jpeg_cbr  = -11059, /**< RA8 JPEG CBR.    */
  k_ra8_jpeg_cbg  = -21709, /**< RA8 JPEG cbg.    */
  k_ra8_jpeg_cbb  = 32768,  /**< RA8 JPEG cbb.    */
  k_ra8_jpeg_crr  = 32768,  /**< RA8 JPEG crr.    */
  k_ra8_jpeg_crg  = -27439, /**< RA8 JPEG crg.    */
  k_ra8_jpeg_crb  = -5329,  /**< RA8 JPEG crb.    */
  k_ra8_jpeg_cr_r = 91881,  /**< 1.40200.         */
  k_ra8_jpeg_cb_b = 116130, /**< 1.77200.         */
  k_ra8_jpeg_cr_g = -46802, /**< RA8 JPEG cr g.   */
  k_ra8_jpeg_cb_g = -22554, /**< RA8 JPEG cb g.   */
} ra8_jpeg_color_coef_t;

/**
 * @brief Zig-zag de-interleave order (T.81 Figure 5).
 *
 * @details
 * `s_zigzag[i]` returns the row-major 0..63 position that the
 * i-th transmitted coefficient lives at inside an 8x8 block.
 * Read-only; each translation unit holds its own copy.
 */
static const uint8_t s_zigzag[64] = {
  0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
  41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
  30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/**
 * @brief Cosine table cos((2n+1)*k*pi/16) * 2^14, k = row, n = col.
 *
 * @details
 * Shared by both forward and inverse 1-D DCT so the encoder and
 * decoder are guaranteed to use a numerically identical basis. Q14
 * picks the largest scale that lets a row sum fit in int32_t for
 * 16-bit pre-normalized inputs. Read-only; each translation unit
 * holds its own copy.
 */
static const int32_t s_dct_cos_q14[8][8] = {
  {16384, 16384, 16384, 16384, 16384, 16384, 16384, 16384},
  {16069, 13623, 9102, 3196, -3196, -9102, -13623, -16069},
  {15137, 6270, -6270, -15137, -15137, -6270, 6270, 15137},
  {13623, -3196, -16069, -9102, 9102, 16069, 3196, -13623},
  {11585, -11585, -11585, 11585, 11585, -11585, -11585, 11585},
  {9102, -16069, 3196, 13623, -13623, -3196, 16069, -9102},
  {6270, -15137, 15137, -6270, -6270, 15137, -15137, 6270},
  {3196, -9102, 13623, -16069, 16069, -13623, 9102, -3196},
};

/**
 * @brief sqrt(2/N)*C(k) DCT normalization weights, Q14.
 *
 * @details
 * The cosine table is Q14; this sqrt(2/N)*C(k) weight is also Q14
 * so the inner accumulator is Q28, which the DCT row loops shift
 * back to Q0 with a `>>14` round on emit. Read-only; each
 * translation unit holds its own copy.
 */
static const int32_t s_dct_w_q14[8] = {
  /* sqrt(2/8) = 0.5; * (1/sqrt(2)) for k=0 -> 0.3536068; * 16384 = 5793. */
  5793,
  /* sqrt(2/8) = 0.5; * 16384 = 8192 (k=1..7). */
  8192,
  8192,
  8192,
  8192,
  8192,
  8192,
  8192,
};

/* ------------------------------------------------------------------ */
/* Inline byte / pixel helpers */
/* ------------------------------------------------------------------ */

/**
 * @brief Clamp `v` to the unsigned 8-bit pixel range.
 *
 * @details
 * Saturating cast used by the IDCT output stage so out-of-range
 * coefficients map to the legal 8-bit pixel domain. Pure helper.
 *
 * @param[in] v Signed integer value to saturate.
 *
 * @return Saturated 8-bit value.
 * @retval 0      ``v`` was negative.
 * @retval 255    ``v`` was > 255.
 * @retval (u8)v  ``v`` was in 0..255.
 *
 * @pre ``v`` is a valid signed integer (no preconditions on range).
 * @pre Caller is using the result for an 8-bit pixel slot.
 * @post Return value is in 0..255 (k_ra8_jpeg_pixel_max).
 * @post No global state is touched.
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL static inline uint8_t internal_clamp_u8(int32_t v)
{
  if (v < 0) {
    return 0U;
  }
  if (v > (int32_t)k_ra8_jpeg_pixel_max) {
    return (uint8_t)k_ra8_jpeg_pixel_max;
  }
  return (uint8_t)v;
}

/**
 * @brief Read a 16-bit big-endian word from `p`.
 *
 * @details
 * JPEG marker payloads use big-endian length prefixes. Inline helper
 * keeps the parser sites free of byte-shifting noise.
 *
 * @param[in] p Non-NULL pointer to two readable bytes.
 *
 * @return Decoded big-endian word.
 * @retval 0..65535 ``(p[0] << 8) | p[1]``.
 *
 * @pre ``p`` is non-NULL.
 * @pre At least two bytes are readable starting at ``p``.
 * @post No memory has been written.
 * @post Return value reflects the bytes at the call site.
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL static inline uint16_t internal_read_be16(const uint8_t* p)
{
  return (uint16_t)(((uint16_t)p[0] << k_ra8_jpeg_byte_shift) | (uint16_t)p[1]);
}

/**
 * @brief Write a 16-bit big-endian word into `p`.
 *
 * @details
 * Encoder helper for emitting JPEG marker payloads (length prefixes,
 * SOFn / DHT / DQT field counts).
 *
 * @param[out] p Non-NULL pointer to two writable bytes.
 * @param[in]  v Value to encode.
 *
 * @pre ``p`` is non-NULL.
 * @pre At least two bytes are writable starting at ``p``.
 * @post ``p[0]`` holds the high byte of ``v``, ``p[1]`` the low byte.
 * @post No global state is touched.
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
RA8_INTERNAL static inline void internal_write_be16(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)(v >> k_ra8_jpeg_byte_shift);
  p[1] = (uint8_t)(v & k_jpeg_byte_mask);
}

/* ------------------------------------------------------------------ */
/* Shared entropy / DSP primitive types */
/* ------------------------------------------------------------------ */

/**
 * @struct ra8_jpeg_bitreader_t
 * @brief Big-endian bit reader over an entropy-coded segment.
 *
 * @details
 * Tracks a streaming byte cursor plus a 32-bit accumulator with
 * `nbits` valid bits at its MSB end. Handles `0xFF 0x00` byte
 * stuffing per T.81 sec F.1.2.3.
 */
typedef struct {
  const uint8_t* buf;     /**< Byte stream.                        */
  uint32_t       len;     /**< Total bytes.                        */
  uint32_t       pos;     /**< Read cursor.                        */
  uint32_t       acc;     /**< Bit accumulator.                    */
  uint8_t        nbits;   /**< Bits valid in `acc` (0..32).        */
  uint8_t        had_eoi; /**< Set when an end marker is consumed. */
} ra8_jpeg_bitreader_t;

/**
 * @struct ra8_jpeg_htab_t
 * @brief Decoded Huffman table -- 256 entries indexed by symbol order.
 *
 * @details
 * Stores the canonical Huffman code for each symbol and the
 * `mincode`/`maxcode` arrays from T.81 Annex C/F that drive the
 * symbol lookup.
 */
typedef struct {
  uint8_t  bits[k_ra8_jpeg_huff_lengths];    /**< BITS list.       */
  uint8_t  vals[k_ra8_jpeg_huff_max];        /**< Symbol order.    */
  uint16_t huffcode[k_ra8_jpeg_huff_max];    /**< Code per symbol. */
  uint8_t  huffsize[k_ra8_jpeg_huff_max];    /**< Code length.     */
  int32_t  mincode[k_ra8_jpeg_huff_lengths]; /**< Mincode.         */
  int32_t  maxcode[k_ra8_jpeg_huff_lengths]; /**< Maxcode.         */
  uint16_t valptr[k_ra8_jpeg_huff_lengths];  /**< Valptr.          */
  uint16_t total;                            /**< Total.           */
} ra8_jpeg_htab_t;

/* ------------------------------------------------------------------ */
/* Decode-primitive prototypes (defined in ra8_jpeg_sw.c, called from */
/* the parser unit ra8_jpeg_sw_decode.c) */
/* ------------------------------------------------------------------ */

/**
 * @brief Pop `n` bits MSB-first; returns -1 on underflow.
 *
 * @details
 * Tops off the accumulator from the entropy stream, then drains `n`
 * MSBs as a non-negative integer. Defined in `ra8_jpeg_sw.c`; the
 * parser unit calls it while decoding DC/AC magnitude bits.
 *
 * @param[in,out] br Bit reader (state mutated in place).
 * @param[in]     n  Number of bits to consume (0..16).
 *
 * @return Decoded bit pattern, or -1 on stream underflow.
 * @retval >=0 ``n``-bit unsigned value drained from the accumulator.
 * @retval -1  Underflow / EOI before ``n`` bits were available.
 *
 * @pre ``br`` is non-NULL.
 * @pre ``n`` <= 16 (caller-enforced).
 * @post ``br->nbits`` decreases by ``n`` on success.
 * @post Accumulator is masked to its remaining bits.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV int32_t priv_jpeg_sw_br_get_bits(ra8_jpeg_bitreader_t* br, uint8_t n);

/**
 * @brief Build canonical code/size and mincode/maxcode tables.
 *
 * @details
 * Implements T.81 Annex C "Generation of size table" + "Generation of
 * code table" plus the Annex F.2.2.3 mincode/maxcode/valptr tables
 * used by the symbol decoder. Defined in `ra8_jpeg_sw.c`; the parser
 * unit calls it from `dec_parse_dht()`.
 *
 * @param[in,out] h Huffman table (BITS / VALS in, derived tables out).
 *
 * @pre ``h`` is non-NULL.
 * @pre ``h->bits`` and ``h->vals`` populated from the JPEG DHT marker.
 * @post ``h->huffcode``, ``h->huffsize``, ``h->mincode``, ``h->maxcode``
 *       and ``h->valptr`` are populated.
 * @post ``h->total`` reflects the symbol count.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV void priv_jpeg_sw_htab_build(ra8_jpeg_htab_t* h);

/**
 * @brief Decode one Huffman symbol from `br` using table `h`.
 *
 * @details
 * Single-bit greedy lookup per T.81 F.2.2.3 "Decoder code-length
 * algorithm". Returns -1 on stream underflow or table miss. Defined
 * in `ra8_jpeg_sw.c`; the parser unit calls it from `dec_block()`.
 *
 * @param[in,out] br Bit reader (state mutated in place).
 * @param[in]     h  Pre-built canonical Huffman table.
 *
 * @return Decoded symbol or -1 on error.
 * @retval >=0 Symbol value from ``h->vals``.
 * @retval -1  Stream underflow or table miss.
 *
 * @pre ``br`` and ``h`` non-NULL.
 * @pre ``h`` was previously populated by ``priv_jpeg_sw_htab_build``.
 * @post ``br`` advances by the consumed code length on success.
 * @post No table state is mutated.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV int32_t priv_jpeg_sw_htab_decode(ra8_jpeg_bitreader_t* br, const ra8_jpeg_htab_t* h);

/*
 * The decoder's remaining primitives, its parse context and its
 * marker-action enum were declared here while the codec was C. The codec is
 * Zig and they are internal to it now (src/internal/dec_ctx.zig
 * and its neighbours), so they are no longer declared. What is left is the
 * surface tests/graphics/src/test_ra8_jpeg_sw_cov.c drives directly: the two
 * entropy structs by value, and the three primitives above.
 */
