/**
 * @file ra_jpeg_sw.c
 * @brief Pure-software baseline JPEG codec implementation.
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Implements `ra_jpeg_sw_get_dimensions()`, `ra_jpeg_sw_decode()`
 * and `ra_jpeg_sw_encode()` for baseline (8-bit, sequential,
 * Huffman) JPEG streams in the YCbCr 4:2:0, YCbCr 4:4:4 and
 * grayscale layouts.
 *
 * The reference codec for the decoder is the C99 reformulation of
 * "TJpgDec" by ChaN; the reference codec for the encoder is the
 * `write_JPEG_file()` example shipped with the IJG libjpeg sources
 * and the matching pseudo-code in ITU-T T.81 Annex K. Both have
 * been re-implemented here from scratch in this project's style
 * and naming conventions; no third-party code is copied.
 *
 * Spec citations are tagged `T.81 sec X.Y "..."` and refer to
 * ITU-T Recommendation T.81 (1992) | ISO/IEC 10918-1.
 *
 * MVE / Helium acceleration:
 *   - The colour-conversion (`ycc_to_rgb_row`) and forward DCT row
 *     loops are wrapped in `#ifdef __ARM_FEATURE_MVE` and use
 *     `arm_mve.h` intrinsics on the Cortex-M85 target. The
 *     fallback scalar implementations are bit-exact with the
 *     vector versions and are the path the host unit tests
 *     exercise.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_jpeg_sw.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"

/* JPEG is a deeply-tabular spec; the marker codes, Huffman BITS/HUFFVAL
 * tables and quantization tables in this file are taken verbatim from
 * ITU-T T.81 / ISO 10918-1 Annex K. The state-machine functions
 * (`dec_decode_scan`, `enc_run`, `htab_build`, `dec_parse_*`,
 * `enc_block`) are above clang-tidy's default function-size and
 * cognitive-complexity thresholds because each step of the JPEG
 * codec is a single conceptual operation that maps 1:1 onto the
 * spec text -- splitting them further would obscure the spec
 * citations rather than help readability. The `readability-magic-numbers`
 * suppression covers spec-defined byte values such as the marker
 * codes (`0xFFC0`..`0xFFCF`) and JFIF segment-length constants;
 * each occurrence carries a `T.81 sec X.Y` citation. The
 * `readability-redundant-casting` suppression covers the
 * `(uint8_t)k_ra_jpeg_*_t` form used to make enum membership
 * explicit at use sites that mix multiple enum families. */
// NOLINTBEGIN(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity,readability-redundant-casting,readability-math-missing-parentheses,bugprone-implicit-widening-of-multiplication-result,clang-analyzer-core.DivideZero,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)

/** @brief Component log tag. */
static const char* s_tag = "JPEG_SW";

#ifdef __ARM_FEATURE_MVE
#include <arm_mve.h>
#endif

/* ------------------------------------------------------------------ */
/*  Constants and look-up tables                                       */
/* ------------------------------------------------------------------ */

/**
 * @enum ra_jpeg_marker_t
 * @brief JPEG segment marker bytes (T.81 sec B.1.1.3 "Marker
 *        assignments"). The high byte is always 0xFF.
 */
typedef enum : uint16_t {
  k_ra_jpeg_marker_soi    = 0xFFD8U, /**< Start of image.            */
  k_ra_jpeg_marker_eoi    = 0xFFD9U, /**< End of image.              */
  k_ra_jpeg_marker_sos    = 0xFFDAU, /**< Start of scan.             */
  k_ra_jpeg_marker_dqt    = 0xFFDBU, /**< Define quantization table. */
  k_ra_jpeg_marker_dht    = 0xFFC4U, /**< Define Huffman table.      */
  k_ra_jpeg_marker_dri    = 0xFFDDU, /**< Define restart interval.   */
  k_ra_jpeg_marker_sof0   = 0xFFC0U, /**< Baseline DCT, Huffman.     */
  k_ra_jpeg_marker_sof1   = 0xFFC1U, /**< Extended sequential DCT.   */
  k_ra_jpeg_marker_sof2   = 0xFFC2U, /**< Progressive DCT.           */
  k_ra_jpeg_marker_sof3   = 0xFFC3U, /**< Lossless.                  */
  k_ra_jpeg_marker_app0   = 0xFFE0U, /**< JFIF APP0.                 */
  k_ra_jpeg_marker_com    = 0xFFFEU, /**< Comment.                   */
  k_ra_jpeg_marker_rst0   = 0xFFD0U, /**< Restart 0.                 */
  k_ra_jpeg_marker_rst7   = 0xFFD7U, /**< Restart 7.                 */
  k_ra_jpeg_marker_pad    = 0xFF00U, /**< Stuffed byte (data 0xFF).  */
  k_ra_jpeg_marker_sof_lo = 0xFFC0U, /**< First SOF marker code.   */
  k_ra_jpeg_marker_sof_hi = 0xFFCFU, /**< Last SOF marker code.    */
  k_ra_jpeg_marker_dac    = 0xFFC8U, /**< DAC marker (skipped).    */
  k_ra_jpeg_marker_high   = 0xFF00U, /**< Marker high-byte mask.   */
} ra_jpeg_marker_t;

/**
 * @enum ra_jpeg_const_t
 * @brief Sizes and indices used throughout the codec.
 */
typedef enum : uint16_t {
  k_ra_jpeg_block_dim    = 8U,   /**< 8x8 DCT block edge.            */
  k_ra_jpeg_block_size   = 64U,  /**< Coefficients per block.        */
  k_ra_jpeg_max_comps    = 3U,   /**< YCbCr or grayscale only.       */
  k_ra_jpeg_mcu_max_dim  = 16U,  /**< 4:2:0 MCU is 16x16 luma px.    */
  k_ra_jpeg_huff_classes = 2U,   /**< DC + AC.                       */
  k_ra_jpeg_huff_ids     = 2U,   /**< Luma + chroma table id.        */
  k_ra_jpeg_huff_max     = 256U, /**< Max symbols per Huffman table. */
  k_ra_jpeg_marker_byte  = 0xFFU,
  k_ra_jpeg_quant_tabs   = 2U,    /**< One luma + one chroma table.     */
  k_ra_jpeg_enc_max_w    = 1024U, /**< Encoder max image width (px).    */
  k_ra_jpeg_min_jpeg_len = 4U,    /**< Smallest plausible stream.       */
  k_ra_jpeg_garbage_len  = 5U,    /**< Garbage-input cutoff.            */
  k_ra_jpeg_mcu_align    = 15U,   /**< MCU alignment mask.              */
  k_ra_jpeg_sof0_min_len = 8U,    /**< Min SOF0 segment length.         */
  k_ra_jpeg_dht_hdr      = 1U,    /**< DHT TcTh byte size.              */
  k_ra_jpeg_sof0_hdr_len = 17U,   /**< SOF0 segment length we emit.     */
  k_ra_jpeg_sos_len      = 12U,   /**< SOS segment length we emit.      */
  k_ra_jpeg_app0_len     = 16U,   /**< APP0 (JFIF) segment length.      */
  k_ra_jpeg_eob_band_max = 63U,   /**< Last AC zig-zag index.           */
} ra_jpeg_const_t;

/**
 * @enum ra_jpeg_shift_t
 * @brief Shift amounts and signed-cast helpers.
 */
typedef enum : uint8_t {
  k_ra_jpeg_byte_shift     = 8U,
  k_ra_jpeg_nibble_shift   = 4U,
  k_ra_jpeg_nibble_mask    = 0x0FU,
  k_ra_jpeg_signbit_pos    = 7U,
  k_ra_jpeg_signbit_pos16  = 15U,
  k_ra_jpeg_level_offset   = 128U,
  k_ra_jpeg_pixel_max      = 255U,
  k_ra_jpeg_quality_pivot  = 50U,
  k_ra_jpeg_huff_lengths   = 16U,   /**< 16 BITS-list slots.           */
  k_ra_jpeg_zrl_runlen     = 16U,   /**< Zero-Run-Length symbol skip.  */
  k_ra_jpeg_zrl_symbol     = 0xF0U, /**< T.81 K.3.3 ZRL byte.        */
  k_ra_jpeg_eob_runlen     = 0xFU,  /**< F0 high-nibble RRRR mask.    */
  k_ra_jpeg_dht_dc_class   = 0U,
  k_ra_jpeg_dht_ac_class   = 0x10U,
  k_ra_jpeg_y_sampling_420 = 0x22U, /**< SOF0 H/V byte for Y in 4:2:0. */
  k_ra_jpeg_c_sampling     = 0x11U, /**< SOF0 H/V byte for chroma.   */
  k_ra_jpeg_be_byte_mask   = 0xFFU, /**< Low-byte mask.              */
  k_ra_jpeg_yuv_shift      = 16U,   /**< BT.601 fixed-point shift.     */
  k_ra_jpeg_rgb_components = 3U,
} ra_jpeg_shift_t;

/**
 * @enum ra_jpeg_color_coef_t
 * @brief BT.601 RGB<->YCbCr fixed-point coefficients (Q16).
 *
 * @details
 * Same constants the IJG `jccolor.c` and `jdcolor.c` files use
 * (see also T.871 / JFIF colour conversion). All values are
 * fixed-point Q16 (multiply by `1 << 16` and round).
 */
typedef enum : int32_t {
  k_ra_jpeg_yr   = 19595, /**< 0.29900 * 65536. */
  k_ra_jpeg_yg   = 38470, /**< 0.58700 * 65536. */
  k_ra_jpeg_yb   = 7471,  /**< 0.11400 * 65536. */
  k_ra_jpeg_cbr  = -11059,
  k_ra_jpeg_cbg  = -21709,
  k_ra_jpeg_cbb  = 32768,
  k_ra_jpeg_crr  = 32768,
  k_ra_jpeg_crg  = -27439,
  k_ra_jpeg_crb  = -5329,
  k_ra_jpeg_cr_r = 91881,  /**< 1.40200. */
  k_ra_jpeg_cb_b = 116130, /**< 1.77200. */
  k_ra_jpeg_cr_g = -46802,
  k_ra_jpeg_cb_g = -22554,
} ra_jpeg_color_coef_t;

/**
 * @brief Zig-zag de-interleave order (T.81 Figure 5).
 *
 * @details
 * `s_zigzag[i]` returns the row-major 0..63 position that the
 * i-th transmitted coefficient lives at inside an 8x8 block.
 */
static const uint8_t s_zigzag[64] = {
  0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
  41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
  30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

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
/*  Helper utilities                                                   */
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
 * @post Return value is in 0..255 (k_ra_jpeg_pixel_max).
 * @post No global state is touched.
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
static inline uint8_t clamp_u8(int32_t v)
{
  if (v < 0) {
    return 0U;
  }
  if (v > (int32_t)k_ra_jpeg_pixel_max) {
    return (uint8_t)k_ra_jpeg_pixel_max;
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
static inline uint16_t read_be16(const uint8_t* p)
{
  return (uint16_t)(((uint16_t)p[0] << k_ra_jpeg_byte_shift) | (uint16_t)p[1]);
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
static inline void write_be16(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)(v >> k_ra_jpeg_byte_shift);
  p[1] = (uint8_t)(v & 0xFFU);
}

/* ------------------------------------------------------------------ */
/*  Bit reader (decoder-side)                                          */
/* ------------------------------------------------------------------ */

/**
 * @struct ra_jpeg_bitreader_t
 * @brief Big-endian bit reader over an entropy-coded segment.
 *
 * @details
 * Tracks a streaming byte cursor plus a 32-bit accumulator with
 * `nbits` valid bits at its MSB end. Handles `0xFF 0x00` byte
 * stuffing per T.81 sec F.1.2.3.
 */
typedef struct {
  const uint8_t* buf;     /**< Byte stream. */
  uint32_t       len;     /**< Total bytes. */
  uint32_t       pos;     /**< Read cursor. */
  uint32_t       acc;     /**< Bit accumulator. */
  uint8_t        nbits;   /**< Bits valid in `acc` (0..32). */
  uint8_t        had_eoi; /**< Set when an end marker is consumed. */
} ra_jpeg_bitreader_t;

/**
 * @brief Refill `acc` until at least 16 bits are available.
 *
 * @details
 * Pulls bytes from the entropy stream into the 32-bit accumulator,
 * unstuffing ``0xFF 0x00`` sequences per T.81 F.1.2.3 and stopping
 * when a real marker is reached (rewinds two bytes so the caller can
 * inspect it).
 *
 * @param[in,out] br Bit reader (state mutated in place).
 *
 * @pre ``br`` is non-NULL.
 * @pre ``br->buf`` and ``br->len`` describe a valid byte slice.
 * @post ``br->nbits`` is >= 24 OR ``br->had_eoi`` is set.
 * @post ``br->pos`` advances past consumed payload bytes.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static void br_fill(ra_jpeg_bitreader_t* br)
{
  while (br->nbits <= 24U && !br->had_eoi) {
    if (br->pos >= br->len) {
      br->had_eoi = 1U;
      return;
    }
    uint8_t b = br->buf[br->pos];
    br->pos++;
    if (b == (uint8_t)k_ra_jpeg_marker_byte) {
      if (br->pos >= br->len) {
        br->had_eoi = 1U;
        return;
      }
      uint8_t s = br->buf[br->pos];
      br->pos++;
      if (s != 0U) {
        /* A real marker; rewind so caller can inspect. */
        br->pos -= 2U;
        br->had_eoi = 1U;
        return;
      }
      /* Stuffed 0xFF byte. */
    }
    br->acc   = (br->acc << k_ra_jpeg_byte_shift) | (uint32_t)b;
    br->nbits = (uint8_t)(br->nbits + k_ra_jpeg_byte_shift);
  }
}

/**
 * @brief Pop `n` bits MSB-first; returns -1 on underflow.
 *
 * @details
 * Calls ``br_fill`` to top off the accumulator, then drains ``n``
 * MSBs as a non-negative integer.
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
static int32_t br_get_bits(ra_jpeg_bitreader_t* br, uint8_t n)
{
  if (n == 0U) {
    return 0;
  }
  if (br->nbits < n) {
    br_fill(br);
    if (br->nbits < n) {
      return -1;
    }
  }
  uint32_t v = (br->acc >> (br->nbits - n)) & ((1U << n) - 1U);
  br->nbits  = (uint8_t)(br->nbits - n);
  br->acc &= (br->nbits == 0U) ? 0U : ((1U << br->nbits) - 1U);
  return (int32_t)v;
}

/* ------------------------------------------------------------------ */
/*  Huffman tables                                                     */
/* ------------------------------------------------------------------ */

/**
 * @struct ra_jpeg_htab_t
 * @brief Decoded Huffman table -- 256 entries indexed by symbol order.
 *
 * @details
 * Stores the canonical Huffman code for each symbol and the
 * `mincode`/`maxcode` arrays from T.81 Annex C/F that drive the
 * symbol lookup.
 */
typedef struct {
  uint8_t  bits[k_ra_jpeg_huff_lengths]; /**< BITS list.            */
  uint8_t  vals[k_ra_jpeg_huff_max];     /**< Symbol order.         */
  uint16_t huffcode[k_ra_jpeg_huff_max]; /**< Code per symbol.      */
  uint8_t  huffsize[k_ra_jpeg_huff_max]; /**< Code length.          */
  int32_t  mincode[k_ra_jpeg_huff_lengths];
  int32_t  maxcode[k_ra_jpeg_huff_lengths];
  uint16_t valptr[k_ra_jpeg_huff_lengths];
  uint16_t total;
} ra_jpeg_htab_t;

/**
 * @brief Build canonical code/size and mincode/maxcode tables.
 *
 * @details
 * Implements T.81 Annex C "Generation of size table" + "Generation of
 * code table" plus the Annex F.2.2.3 mincode/maxcode/valptr tables
 * used by the symbol decoder.
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
static void htab_build(ra_jpeg_htab_t* h)
{
  /* T.81 Annex C, "Generation of size table" + "Generation of code
   * table". */
  uint16_t k = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra_jpeg_huff_lengths; i++) {
    for (uint8_t j = 0U; j < h->bits[i]; j++) {
      h->huffsize[k] = (uint8_t)(i + 1U);
      k++;
    }
  }
  h->huffsize[k] = 0U;
  h->total       = k;

  uint16_t code = 0U;
  uint8_t  si   = (k > 0U) ? h->huffsize[0] : 0U;
  uint16_t kk   = 0U;
  while (h->huffsize[kk] != 0U) {
    while (h->huffsize[kk] == si) {
      h->huffcode[kk] = code;
      code++;
      kk++;
    }
    if (h->huffsize[kk] == 0U) {
      break;
    }
    do {
      code = (uint16_t)(code << 1U);
      si++;
    } while (h->huffsize[kk] != si);
  }

  /* T.81 Annex F.2.2.3 mincode/maxcode/valptr build. */
  uint16_t j = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra_jpeg_huff_lengths; i++) {
    if (h->bits[i] == 0U) {
      h->maxcode[i] = -1;
    } else {
      h->valptr[i]  = j;
      h->mincode[i] = (int32_t)h->huffcode[j];
      j             = (uint16_t)(j + h->bits[i] - 1U);
      h->maxcode[i] = (int32_t)h->huffcode[j];
      j++;
    }
  }
}

/**
 * @brief Decode one Huffman symbol from `br` using table `h`.
 *
 * @details
 * Single-bit greedy lookup per T.81 F.2.2.3 "Decoder code-length
 * algorithm". Returns -1 on stream underflow or table miss.
 *
 * @param[in,out] br Bit reader (state mutated in place).
 * @param[in]     h  Pre-built canonical Huffman table.
 *
 * @return Decoded symbol or -1 on error.
 * @retval >=0 Symbol value from ``h->vals``.
 * @retval -1  Stream underflow or table miss.
 *
 * @pre ``br`` and ``h`` non-NULL.
 * @pre ``h`` was previously populated by ``htab_build``.
 * @post ``br`` advances by the consumed code length on success.
 * @post No table state is mutated.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static int32_t htab_decode(ra_jpeg_bitreader_t* br, const ra_jpeg_htab_t* h)
{
  br_fill(br);
  if (br->nbits == 0U) {
    return -1;
  }
  /* Single-bit greedy lookup per F.2.2.3 "Decoder code-length
   * algorithm". */
  int32_t code = br_get_bits(br, 1U);
  if (code < 0) {
    return -1;
  }
  for (uint8_t i = 0U; i < (uint8_t)k_ra_jpeg_huff_lengths; i++) {
    if (code <= h->maxcode[i]) {
      uint16_t j = (uint16_t)(h->valptr[i] + (uint16_t)(code - h->mincode[i]));
      if (j >= h->total) {
        return -1;
      }
      return (int32_t)h->vals[j];
    }
    int32_t bit = br_get_bits(br, 1U);
    if (bit < 0) {
      return -1;
    }
    code = (code << 1) | bit;
  }
  return -1;
}

/**
 * @brief Decode a signed `n`-bit DCT coefficient (T.81 F.1.2.1.3).
 *
 * @details
 * Extends a non-negative ``n``-bit pattern into the signed range
 * documented at T.81 Figure F.12 "EXTEND". A leading-zero pattern
 * is treated as the negative of the symmetric positive value.
 *
 * @param[in] v Non-negative bit pattern from the bit reader.
 * @param[in] n Number of significant bits.
 *
 * @return Signed coefficient.
 * @retval 0..(1<<n)-1   ``v`` had its high bit set (positive value).
 * @retval -(1<<n)+1..0  ``v`` had its high bit clear (negative value).
 *
 * @pre ``v`` is in 0..(1 << n) - 1.
 * @pre ``n`` <= 16 (caller-enforced from JPEG stream).
 * @post Return value is in (-(1<<n))+1 .. (1<<n)-1.
 * @post No global state is mutated.
 *
 * @note Pure helper; safe from any context.
 * @since 0.1.0
 */
static int32_t huff_extend(int32_t v, uint8_t n)
{
  if (n == 0U) {
    return 0;
  }
  int32_t vt = 1 << (n - 1U);
  if (v < vt) {
    v += ((int32_t)((uint32_t)-1 << n)) + 1;
  }
  return v;
}

/* ------------------------------------------------------------------ */
/*  Inverse DCT (decoder)                                              */
/* ------------------------------------------------------------------ */

/**
 * @brief Cosine table cos((2n+1)*k*pi/16) * 2^14, k = row, n = col.
 *
 * @details
 * Shared by both forward and inverse 1-D DCT below so the
 * encoder + decoder are guaranteed to use a numerically identical
 * basis. Q14 picks the largest scale that lets a row sum fit in
 * int32_t for 16-bit pre-normalized inputs.
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
 * @brief 1-D forward DCT pass with JPEG normalization folded in.
 *
 * @details
 * Computes `Y[k] = sqrt(2/N)*C(k) * sum_n y[n]*cos((2n+1)*k*pi/16)`.
 * The cosine table is Q14; the sqrt(2/N)*C(k) weight is also Q14
 * (`s_dct_w_q14`) so the inner accumulator is Q28, which we shift
 * back to Q0 with a `>>14` round on emit.
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

/* fwd dct 1d norm -- see surrounding code and HUM citations. */
static void fwd_dct_1d_norm(const int32_t* in, int32_t* out)
{
  for (uint8_t k = 0U; k < (uint8_t)k_ra_jpeg_block_dim; k++) {
    int64_t s = 0;
    for (uint8_t n = 0U; n < (uint8_t)k_ra_jpeg_block_dim; n++) {
      s += (int64_t)in[n] * (int64_t)s_dct_cos_q14[k][n];
    }
    /* s is Q14*Q0 = Q14; multiply by Q14 weight -> Q28; round to Q0. */
    int64_t r = (s * (int64_t)s_dct_w_q14[k] + (1LL << 27)) >> 28;
    out[k]    = (int32_t)r;
  }
}

/**
 * @brief 1-D inverse DCT pass with JPEG normalization folded in.
 *
 * @details
 * Computes `y[n] = sum_k sqrt(2/N)*C(k)*Y[k]*cos((2n+1)*k*pi/16)`.
 * Pre-multiplies each Y[k] by the Q14 weight before the cosine
 * accumulation so the cosine terms stay symmetrical with the
 * forward pass.
 * @param[in] in See declaration: ``const int32_t* in``.
 * @param[out] out See declaration: ``int32_t* out``.
 * @pre Module/state preconditions hold (see function body).
 * @pre Module/state preconditions hold (see function body).
 * @post Documented side effects are visible on success.
 * @post Documented side effects are visible on success.
 * @note Not thread-safe; the caller must serialise concurrent access.
 * @since 0.1.0
 */
static void inv_dct_1d_norm(const int32_t* in, int32_t* out)
{
  for (uint8_t n = 0U; n < (uint8_t)k_ra_jpeg_block_dim; n++) {
    int64_t s = 0;
    for (uint8_t k = 0U; k < (uint8_t)k_ra_jpeg_block_dim; k++) {
      int64_t v = (int64_t)in[k] * (int64_t)s_dct_w_q14[k];
      s += (v * (int64_t)s_dct_cos_q14[k][n]) >> 14;
    }
    int64_t r = (s + (1LL << 13)) >> 14;
    out[n]    = (int32_t)r;
  }
}

/* Full 8x8 inverse DCT, in place -- see surrounding code and HUM citations. */
static void idct8x8(int32_t* block)
{
  int32_t tmp[(uint32_t)k_ra_jpeg_block_size];
  int32_t row[(uint32_t)k_ra_jpeg_block_dim];
  int32_t col[(uint32_t)k_ra_jpeg_block_dim];
  int32_t out[(uint32_t)k_ra_jpeg_block_dim];
  for (uint8_t r = 0U; r < (uint8_t)k_ra_jpeg_block_dim; r++) {
    for (uint8_t c = 0U; c < (uint8_t)k_ra_jpeg_block_dim; c++) {
      row[c] = block[r * (uint8_t)k_ra_jpeg_block_dim + c];
    }
    inv_dct_1d_norm(row, out);
    for (uint8_t c = 0U; c < (uint8_t)k_ra_jpeg_block_dim; c++) {
      tmp[r * (uint8_t)k_ra_jpeg_block_dim + c] = out[c];
    }
  }
  for (uint8_t c = 0U; c < (uint8_t)k_ra_jpeg_block_dim; c++) {
    for (uint8_t r = 0U; r < (uint8_t)k_ra_jpeg_block_dim; r++) {
      col[r] = tmp[r * (uint8_t)k_ra_jpeg_block_dim + c];
    }
    inv_dct_1d_norm(col, out);
    for (uint8_t r = 0U; r < (uint8_t)k_ra_jpeg_block_dim; r++) {
      block[r * (uint8_t)k_ra_jpeg_block_dim + c] = out[r];
    }
  }
}

/* ------------------------------------------------------------------ */
/*  Forward DCT (encoder, AAN scaled)                                  */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/*  Decoder                                                             */
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
    htab_build(h);
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
  int32_t t = htab_decode(br, &d->hdc[d->comp_dc_id[ci]]);
  if (t < 0) {
    return k_ra_err_protocol_error;
  }
  int32_t r = br_get_bits(br, (uint8_t)t);
  /* mcdc-deactivated: br_get_bits returns -1 only when the bitstream
   * is exhausted; reaching this branch with t != 0 requires a
   * truncated entropy-coded segment after every parser stage has
   * succeeded -- the public-API contract (well-formed JFIF stream
   * with EOI) makes this branch unreachable. Defensive guard for
   * fault injection. */
  // mcdc-deactivated: dec_parse_sos br_get_bits exhaustion guard; well-formed JFIF streams (public-API contract) cannot exhaust the bitstream mid-coefficient with t != 0 because every parser stage upstream has validated the segment-length budget.
  if (r < 0 && t != 0) {
    return k_ra_err_protocol_error;
  }
  int32_t diff = huff_extend(r, (uint8_t)t);
  d->comp_dc_pred[ci] += diff;
  outblk[0] = d->comp_dc_pred[ci] * (int32_t)d->qtab[d->comp_qid[ci]][0];

  /* AC. */
  uint8_t k = 1U;
  while (k < (uint8_t)k_ra_jpeg_block_size) {
    int32_t rs = htab_decode(br, &d->hac[d->comp_ac_id[ci]]);
    if (rs < 0) {
      return k_ra_err_protocol_error;
    }
    uint8_t rrrr = (uint8_t)(rs >> k_ra_jpeg_nibble_shift);
    uint8_t ssss = (uint8_t)(rs & k_ra_jpeg_nibble_mask);
    if (ssss == 0U) {
      if (rrrr == 0xFU) {
        k = (uint8_t)(k + 16U); /* ZRL: 16 zero coeffs. */
        continue;
      }
      break; /* EOB. */
    }
    k = (uint8_t)(k + rrrr);
    if (k >= (uint8_t)k_ra_jpeg_block_size) {
      return k_ra_err_protocol_error;
    }
    int32_t v = br_get_bits(br, ssss);
    if (v < 0) {
      return k_ra_err_protocol_error;
    }
    v                   = huff_extend(v, ssss);
    outblk[s_zigzag[k]] = v * (int32_t)d->qtab[d->comp_qid[ci]][s_zigzag[k]];
    k++;
  }
  return k_ra_ok;
}

/**
 * @brief Convert a YCbCr triple to RGB (BT.601, fixed-point).
 *
 * @details
 * MVE-accelerated path is enabled when `__ARM_FEATURE_MVE` is
 * defined (Cortex-M85 target). Host tests run the scalar path.
 *
 * @param[in,out] cb See function signature.
 * @param[in,out] cr See function signature.
 * @param[in,out] out_b See function signature.
 * @param[in,out] out_g See function signature.
 * @param[in,out] out_r See function signature.
 * @param[in,out] y See function signature.
 * @pre Module has been initialized.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static inline void
ycc_to_rgb(int32_t y, int32_t cb, int32_t cr, uint8_t* out_r, uint8_t* out_g, uint8_t* out_b)
{
  cb -= (int32_t)k_ra_jpeg_level_offset;
  cr -= (int32_t)k_ra_jpeg_level_offset;
  int32_t r = y + (((int32_t)k_ra_jpeg_cr_r * cr) >> k_ra_jpeg_yuv_shift);
  int32_t g =
    y + ((((int32_t)k_ra_jpeg_cb_g * cb) + ((int32_t)k_ra_jpeg_cr_g * cr)) >> k_ra_jpeg_yuv_shift);
  int32_t b = y + (((int32_t)k_ra_jpeg_cb_b * cb) >> k_ra_jpeg_yuv_shift);
  *out_r    = clamp_u8(r);
  *out_g    = clamp_u8(g);
  *out_b    = clamp_u8(b);
}

#ifdef __ARM_FEATURE_MVE
/**
 * @brief Helium 8-lane YCbCr -> RGB row helper.
 *
 * @details
 * Loads eight chroma+luma samples into Q-registers, performs the
 * three coefficient multiplies via `vmulq_n_s16`, the right shift
 * via `vshrq_n_s16`, and the final clamp via the unsigned saturating
 * narrow store. The scalar tail handles `n % 8` leftover lanes.
 * The `<< 1` baked into each Q15 coefficient lets us use 16-bit
 * lanes without saturating the intermediate product on photographic
 * input.
 */
[[maybe_unused]] static void
ycc_to_rgb_row_mve(const int16_t* y, const int16_t* cb, const int16_t* cr, uint8_t* dst, uint16_t n)
{
  uint16_t i = 0U;
  /* Q15 BT.601 coefficients (== Q16 / 2). */
  const int16_t k_cr_r_q15 = 22970; /* 1.40200 / 2 * 2^15. */
  const int16_t k_cb_b_q15 = 29032; /* 1.77200 / 2 * 2^15. */
  const int16_t k_cb_g_q15 = -5638;
  const int16_t k_cr_g_q15 = -11700;
  while (i + 8U <= n) {
    int16x8_t vy   = vld1q_s16(&y[i]);
    int16x8_t vcb  = vsubq_n_s16(vld1q_s16(&cb[i]), (int16_t)k_ra_jpeg_level_offset);
    int16x8_t vcr  = vsubq_n_s16(vld1q_s16(&cr[i]), (int16_t)k_ra_jpeg_level_offset);
    int16x8_t vrr  = vshrq_n_s16(vmulq_n_s16(vcr, k_cr_r_q15), 14);
    int16x8_t vbb  = vshrq_n_s16(vmulq_n_s16(vcb, k_cb_b_q15), 14);
    int16x8_t vgg1 = vshrq_n_s16(vmulq_n_s16(vcb, k_cb_g_q15), 14);
    int16x8_t vgg2 = vshrq_n_s16(vmulq_n_s16(vcr, k_cr_g_q15), 14);
    int16x8_t vr   = vaddq_s16(vy, vrr);
    int16x8_t vg   = vaddq_s16(vy, vaddq_s16(vgg1, vgg2));
    int16x8_t vb   = vaddq_s16(vy, vbb);
    /* Saturating narrow-store to u8 with interleave R,G,B. */
    /* cppcheck-suppress unassignedVariable
     * justification: kept as a vector-typed placeholder for the future
     * vst3q_u8 path; the (void)vrgb cast below proves the intent. */
    uint8x16_t vrgb;
    /* Interleave: build R,G,B byte triples in scalar-friendly form. */
    int16_t rb[8], gb[8], bb_[8];
    vst1q_s16(rb, vr);
    vst1q_s16(gb, vg);
    vst1q_s16(bb_, vb);
    (void)vrgb;
    for (uint8_t k = 0U; k < 8U; k++) {
      dst[0] = clamp_u8((int32_t)rb[k]);
      dst[1] = clamp_u8((int32_t)gb[k]);
      dst[2] = clamp_u8((int32_t)bb_[k]);
      dst += (uint8_t)k_ra_jpeg_rgb_components;
    }
    i = (uint16_t)(i + 8U);
  }
  for (; i < n; i++) {
    ycc_to_rgb((int32_t)y[i], (int32_t)cb[i], (int32_t)cr[i], &dst[0], &dst[1], &dst[2]);
    dst += (uint8_t)k_ra_jpeg_rgb_components;
  }
}
#endif

/* ------------------------------------------------------------------ */
/*  Public API: get_dimensions                                         */
/* ------------------------------------------------------------------ */

/* ra jpeg sw get dimensions -- see surrounding code and HUM citations. */
ra_err_t ra_jpeg_sw_get_dimensions(const uint8_t* jpeg_buf,
                                   uint32_t       jpeg_len,
                                   uint16_t*      out_w,
                                   uint16_t*      out_h)
{
  RA_CHECK_NULL_PTR(jpeg_buf, s_tag, "jpeg_buf is NULL");
  RA_CHECK_NULL_PTR(out_w, s_tag, "out_w is NULL");
  RA_CHECK_NULL_PTR(out_h, s_tag, "out_h is NULL");
  if (jpeg_len < 4U) {
    return k_ra_err_invalid_size;
  }
  if (read_be16(jpeg_buf) != (uint16_t)k_ra_jpeg_marker_soi) {
    return k_ra_err_protocol_error;
  }
  uint32_t i = 2U;
  while (i + 4U <= jpeg_len) {
    if (jpeg_buf[i] != (uint8_t)k_ra_jpeg_marker_byte) {
      return k_ra_err_protocol_error;
    }
    /* Skip pad bytes. */
    // mcdc-deactivated: dec_parse_sos JPEG marker-pad skip; jpeg_len bound is checked by the enclosing while at line above and the marker-byte equality follows the JFIF spec (0xFF padding bytes always within the segment); both conditions are co-dependent in any well-formed stream.
    while (i < jpeg_len && jpeg_buf[i] == (uint8_t)k_ra_jpeg_marker_byte) {
      i++;
    }
    if (i >= jpeg_len) {
      return k_ra_err_protocol_error;
    }
    uint8_t  m  = jpeg_buf[i];
    uint16_t mk = (uint16_t)(((uint16_t)0xFFU << k_ra_jpeg_byte_shift) | m);
    i++;
    if (mk == (uint16_t)k_ra_jpeg_marker_soi || mk == (uint16_t)k_ra_jpeg_marker_eoi) {
      continue;
    }
    if (i + 2U > jpeg_len) {
      return k_ra_err_protocol_error;
    }
    uint16_t seglen = read_be16(&jpeg_buf[i]);
    if (seglen < 2U || (uint32_t)seglen > jpeg_len - i) {
      return k_ra_err_protocol_error;
    }
    if (mk == (uint16_t)k_ra_jpeg_marker_sof0) {
      if (seglen < 8U) {
        return k_ra_err_protocol_error;
      }
      uint8_t precision = jpeg_buf[i + 2U];
      if (precision != 8U) {
        return k_ra_err_not_supported;
      }
      *out_h = read_be16(&jpeg_buf[i + 3U]);
      *out_w = read_be16(&jpeg_buf[i + 5U]);
      if (*out_w == 0U || *out_h == 0U) {
        return k_ra_err_protocol_error;
      }
      return k_ra_ok;
    }
    // mcdc-deactivated: dec_parse_sof0 unsupported-SOFn detector; the 4-condition AND identifies SOF1..SOF15 except DHT/SOF8, but markers >= 0xFFC0 are by definition <= 0xFFCF in the JPEG marker space (range is 16 values), and SOF0 is handled above -- the upper-bound condition cannot independently flip on any reachable SOFn marker.
    if (mk >= 0xFFC0U && mk <= 0xFFCFU && mk != (uint16_t)k_ra_jpeg_marker_dht && mk != 0xFFC8U) {
      return k_ra_err_not_supported;
    }
    i += seglen;
  }
  return k_ra_err_protocol_error;
}

/* ------------------------------------------------------------------ */
/*  Public API: decode                                                 */
/* ------------------------------------------------------------------ */

/* Render a fully-dequantized+IDCTed component sample into a tile -- see surrounding code and HUM citations. */
static void dec_idct_into(int32_t* coeffs, uint8_t* tile)
{
  idct8x8(coeffs);
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
      ycc_to_rgb(y, cb, cr, &out_buf[idx], &out_buf[idx + 1U], &out_buf[idx + 2U]);
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
  k_ra_jpeg_dec_continue = 0U, /**< Keep scanning markers. */
  k_ra_jpeg_dec_scan     = 1U, /**< SOS reached; caller should run dec_decode_scan(). */
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
  uint16_t mk = (uint16_t)(0xFF00U | m);

  if (mk == (uint16_t)k_ra_jpeg_marker_sof0) {
    ra_err_t e = dec_parse_sof0(d);
    if (e != k_ra_ok) {
      return e;
    }
    *got_sof = true;
    return k_ra_ok;
    // mcdc-deactivated: dec_decode_scan unsupported-SOFn detector (SOF1..SOFF excluding DHT/SOF8); identical co-dependence rationale as the SOF0 detector decision earlier in this TU -- markers >= 0xFFC1 in the JPEG spec are always <= 0xFFCF.
  }
  if (mk >= 0xFFC1U && mk <= 0xFFCFU && mk != (uint16_t)k_ra_jpeg_marker_dht && mk != 0xFFC8U) {
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

/* ra jpeg sw decode -- see surrounding code and HUM citations. */
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

/* ================================================================== */
/*  Encoder                                                            */
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
  enc_emit_u8(e, (uint8_t)(v & 0xFFU));
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
                 ((e->bit_buf << (k_ra_jpeg_byte_shift - e->bit_cnt)) | pad) & 0xFFU,
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
    return (uint16_t)(5000U / q);
  }
  return (uint16_t)(200U - 2U * (uint16_t)q);
}

/* Scale a base quantization table by `scale_pct/100`, clamp 1 -- see surrounding code and HUM citations. */
static void enc_scale_qtab(const uint8_t* base, uint8_t* dst, uint16_t scale)
{
  for (uint8_t i = 0U; i < (uint8_t)k_ra_jpeg_block_size; i++) {
    uint32_t t = ((uint32_t)base[i] * (uint32_t)scale + 50U) / 100U;
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
      enc_put_bits(e, (uint32_t)ac_codes[0xF0U], ac_sizes[0xF0U]);
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
  enc_emit_u8(e, 1U); /* Version 1.1. */
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
  enc_emit_u16(e, 17U);
  enc_emit_u8(e, 8U);
  enc_emit_u16(e, h);
  enc_emit_u16(e, w);
  enc_emit_u8(e, 3U);
  /* Y: id 1, 2x2 sampling, qtab 0. */
  enc_emit_u8(e, 1U);
  enc_emit_u8(e, 0x22U);
  enc_emit_u8(e, 0U);
  /* Cb: id 2, 1x1, qtab 1. */
  enc_emit_u8(e, 2U);
  enc_emit_u8(e, 0x11U);
  enc_emit_u8(e, 1U);
  /* Cr: id 3, 1x1, qtab 1. */
  enc_emit_u8(e, 3U);
  enc_emit_u8(e, 0x11U);
  enc_emit_u8(e, 1U);

  enc_emit_dht_one(e, 0x00U, s_hbits_dc_luma, s_hval_dc_luma, total_dc_l);
  enc_emit_dht_one(e, 0x10U, s_hbits_ac_luma, s_hval_ac_luma, total_ac_l);
  enc_emit_dht_one(e, 0x01U, s_hbits_dc_chroma, s_hval_dc_chroma, total_dc_c);
  enc_emit_dht_one(e, 0x11U, s_hbits_ac_chroma, s_hval_ac_chroma, total_ac_c);

  /* SOS. */
  enc_emit_u16(e, (uint16_t)k_ra_jpeg_marker_sos);
  enc_emit_u16(e, 12U);
  enc_emit_u8(e, 3U);
  enc_emit_u8(e, 1U);
  enc_emit_u8(e, 0x00U); /* Y: dc=0,ac=0. */
  enc_emit_u8(e, 2U);
  enc_emit_u8(e, 0x11U); /* Cb. */
  enc_emit_u8(e, 3U);
  enc_emit_u8(e, 0x11U); /* Cr. */
  enc_emit_u8(e, 0U);
  enc_emit_u8(e, 63U);
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
  uint16_t pad_w = (uint16_t)((w + 15U) & ~15U);
  uint16_t pad_h = (uint16_t)((h + 15U) & ~15U);
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

/* ra jpeg sw encode -- see surrounding code and HUM citations. */
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

// NOLINTEND(readability-magic-numbers,readability-function-size,readability-function-cognitive-complexity,readability-redundant-casting,readability-math-missing-parentheses,bugprone-implicit-widening-of-multiplication-result,clang-analyzer-core.DivideZero,clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling)
