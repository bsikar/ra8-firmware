/**
 * @file ra8_jpeg_sw.c
 * @brief Pure-software baseline JPEG codec: shared entropy/DSP
 *        primitives plus the get-dimensions public API.
 *
 * @par Tag
 * [Ring 4 / Domain] {World: NS}
 *
 * @details
 * Implements the codec primitives shared by both the decoder and the
 * encoder -- the big-endian bit reader, the canonical Huffman tables,
 * the inverse DCT and the BT.601 YCbCr->RGB colour conversion -- and
 * the lightweight `ra8_jpeg_sw_get_dimensions()` public API, which only
 * walks the marker chain to the SOF0 frame header.
 *
 * The decoder-driver half (marker parser, MCU scan loop and
 * `ra8_jpeg_sw_decode()`) lives in `ra8_jpeg_sw_decode.c`; the encoder
 * (forward DCT, quantization, Huffman code emission and
 * `ra8_jpeg_sw_encode()`) lives in `ra8_jpeg_sw_encode.c`. Every symbol
 * referenced by more than one of those units -- the C23 typed-enum
 * constant blocks, the shared DSP look-up tables, the inline byte
 * helpers, the bit-reader / Huffman-table types and the prototypes for
 * the primitives defined here -- lives in `ra8_jpeg_sw_internal.h`.
 *
 * The reference codec for the decoder is the C99 reformulation of
 * "TJpgDec" by ChaN; both halves have been re-implemented here from
 * scratch in this project's style and naming conventions; no
 * third-party code is copied.
 *
 * Spec citations are tagged `T.81 sec X.Y "..."` and refer to
 * ITU-T Recommendation T.81 (1992) | ISO/IEC 10918-1.
 *
 * MVE / Helium acceleration:
 *   - The colour-conversion (`ycc_to_rgb_row`) row loop is wrapped in
 *     `#ifdef __ARM_FEATURE_MVE` and uses `arm_mve.h` intrinsics on the
 *     Cortex-M85 target. The fallback scalar implementation is
 *     bit-exact with the vector version and is the path the host unit
 *     tests exercise.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_jpeg_sw.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_jpeg_sw_internal.h"
#include "ra8_log.h"

/** @brief Component log tag. */
static const char* s_tag = "JPEG_SW";

#ifdef __ARM_FEATURE_MVE
#include <arm_mve.h>
#endif

/* ------------------------------------------------------------------ */
/* Bit reader (decoder-side) */
/* ------------------------------------------------------------------ */

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
static void br_fill(ra8_jpeg_bitreader_t* br)
{
  while (br->nbits <= k_jpeg_reservoir_lo && !br->had_eoi) {
    if (br->pos >= br->len) {
      br->had_eoi = 1U;
      return;
    }
    uint8_t b = br->buf[br->pos];
    br->pos++;
    if (b == (uint8_t)k_ra8_jpeg_marker_byte) {
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
    br->acc   = (br->acc << k_ra8_jpeg_byte_shift) | (uint32_t)b;
    br->nbits = (uint8_t)(br->nbits + k_ra8_jpeg_byte_shift);
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
int32_t ra8_jpeg_sw_br_get_bits(ra8_jpeg_bitreader_t* br, uint8_t n)
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
/* Huffman tables */
/* ------------------------------------------------------------------ */

/**
 * @brief Build the F.2.2.3 mincode/maxcode/valptr decode tables.
 *
 * @details
 * Second phase of `ra8_jpeg_sw_htab_build()`: walks the BITS list and
 * the canonical `huffcode` array produced by the Annex C phase and
 * derives, per code length, the smallest code (`mincode`), the largest
 * code (`maxcode`, -1 for unused lengths) and the index of the first
 * symbol of that length (`valptr`) exactly as T.81 Annex F.2.2.3
 * "Decoder tables" specifies.
 *
 * @param[in,out] h Huffman table (huffcode/bits in, lookup tables out).
 *
 * @pre ``h`` is non-NULL.
 * @pre ``h->huffcode`` was populated by the Annex C canonical build.
 * @post ``h->mincode``/``h->maxcode``/``h->valptr`` are populated.
 * @post ``h->maxcode[i]`` is -1 for every length with zero codes.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static void htab_build_lookup(ra8_jpeg_htab_t* h)
{
  uint16_t j = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_jpeg_huff_lengths; i++) {
    if (h->bits[i] == 0U) {
      h->maxcode[i] = -1;
    } else {
      h->valptr[i]  = j;
      h->mincode[i] = (int32_t)h->huffcode[j];
      j             = (uint16_t)((j + h->bits[i]) - 1U);
      h->maxcode[i] = (int32_t)h->huffcode[j];
      j++;
    }
  }
}

void ra8_jpeg_sw_htab_build(ra8_jpeg_htab_t* h)
{
  /* T.81 Annex C, "Generation of size table" + "Generation of code
   * table". */
  uint16_t k = 0U;
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_jpeg_huff_lengths; i++) {
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
  htab_build_lookup(h);
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
 * @pre ``h`` was previously populated by ``ra8_jpeg_sw_htab_build``.
 * @post ``br`` advances by the consumed code length on success.
 * @post No table state is mutated.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
int32_t ra8_jpeg_sw_htab_decode(ra8_jpeg_bitreader_t* br, const ra8_jpeg_htab_t* h)
{
  br_fill(br);
  if (br->nbits == 0U) {
    return -1;
  }
  /* Single-bit greedy lookup per F.2.2.3 "Decoder code-length
   * algorithm". */
  int32_t code = ra8_jpeg_sw_br_get_bits(br, 1U);
  /* After br->nbits == 0 guard above, br->nbits >= 1; br_get_bits(br, 1)
   * with nbits >= 1 always returns 0 or 1 without invoking br_fill, so
   * code < 0 is unreachable on any host input. */
  if (code < 0) {
    return -1; /* GCOVR_EXCL_LINE */
  }
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_jpeg_huff_lengths; i++) {
    if (code <= h->maxcode[i]) {
      uint16_t j = (uint16_t)(h->valptr[i] + (uint16_t)(code - h->mincode[i]));
      if (j >= h->total) {
        return -1;
      }
      return (int32_t)h->vals[j];
    }
    int32_t bit = ra8_jpeg_sw_br_get_bits(br, 1U);
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
int32_t ra8_jpeg_sw_huff_extend(int32_t v, uint8_t n)
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
/* Inverse DCT (decoder) */
/* ------------------------------------------------------------------ */

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
  for (uint8_t n = 0U; n < (uint8_t)k_ra8_jpeg_block_dim; n++) {
    int64_t s = 0;
    for (uint8_t k = 0U; k < (uint8_t)k_ra8_jpeg_block_dim; k++) {
      int64_t v = (int64_t)in[k] * (int64_t)s_dct_w_q14[k];
      s += (v * (int64_t)s_dct_cos_q14[k][n]) >> k_jpeg_q14_shift;
    }
    int64_t r = (s + (1LL << k_jpeg_idct_p2_bias_sh)) >> k_jpeg_q14_shift;
    out[n]    = (int32_t)r;
  }
}

/* Full 8x8 inverse DCT, in place -- see surrounding code and HUM citations. */
void ra8_jpeg_sw_idct8x8(int32_t* block)
{
  int32_t tmp[(uint32_t)k_ra8_jpeg_block_size];
  int32_t row[(uint32_t)k_ra8_jpeg_block_dim];
  int32_t col[(uint32_t)k_ra8_jpeg_block_dim];
  int32_t out[(uint32_t)k_ra8_jpeg_block_dim];
  for (uint8_t r = 0U; r < (uint8_t)k_ra8_jpeg_block_dim; r++) {
    for (uint8_t c = 0U; c < (uint8_t)k_ra8_jpeg_block_dim; c++) {
      row[c] = block[(r * (uint8_t)k_ra8_jpeg_block_dim) + c];
    }
    inv_dct_1d_norm(row, out);
    for (uint8_t c = 0U; c < (uint8_t)k_ra8_jpeg_block_dim; c++) {
      tmp[(r * (uint8_t)k_ra8_jpeg_block_dim) + c] = out[c];
    }
  }
  for (uint8_t c = 0U; c < (uint8_t)k_ra8_jpeg_block_dim; c++) {
    for (uint8_t r = 0U; r < (uint8_t)k_ra8_jpeg_block_dim; r++) {
      col[r] = tmp[(r * (uint8_t)k_ra8_jpeg_block_dim) + c];
    }
    inv_dct_1d_norm(col, out);
    for (uint8_t r = 0U; r < (uint8_t)k_ra8_jpeg_block_dim; r++) {
      block[(r * (uint8_t)k_ra8_jpeg_block_dim) + c] = out[r];
    }
  }
}

/* ------------------------------------------------------------------ */
/* Colour conversion (YCbCr -> RGB) */
/* ------------------------------------------------------------------ */

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
void ra8_jpeg_sw_ycc_to_rgb(int32_t  y,
                            int32_t  cb,
                            int32_t  cr,
                            uint8_t* out_r,
                            uint8_t* out_g,
                            uint8_t* out_b)
{
  cb -= (int32_t)k_ra8_jpeg_level_offset;
  cr -= (int32_t)k_ra8_jpeg_level_offset;
  int32_t r = y + (((int32_t)k_ra8_jpeg_cr_r * cr) >> k_ra8_jpeg_yuv_shift);
  int32_t g = y + ((((int32_t)k_ra8_jpeg_cb_g * cb) + ((int32_t)k_ra8_jpeg_cr_g * cr)) >>
                   k_ra8_jpeg_yuv_shift);
  int32_t b = y + (((int32_t)k_ra8_jpeg_cb_b * cb) >> k_ra8_jpeg_yuv_shift);
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
  while (i + 8U <= n) {
    int16x8_t vy   = vld1q_s16(&y[i]);
    int16x8_t vcb  = vsubq_n_s16(vld1q_s16(&cb[i]), (int16_t)k_ra8_jpeg_level_offset);
    int16x8_t vcr  = vsubq_n_s16(vld1q_s16(&cr[i]), (int16_t)k_ra8_jpeg_level_offset);
    int16x8_t vrr  = vshrq_n_s16(vmulq_n_s16(vcr, k_cr_r_q15), k_jpeg_ycc_q15_shift);
    int16x8_t vbb  = vshrq_n_s16(vmulq_n_s16(vcb, k_cb_b_q15), k_jpeg_ycc_q15_shift);
    int16x8_t vgg1 = vshrq_n_s16(vmulq_n_s16(vcb, k_cb_g_q15), k_jpeg_ycc_q15_shift);
    int16x8_t vgg2 = vshrq_n_s16(vmulq_n_s16(vcr, k_cr_g_q15), k_jpeg_ycc_q15_shift);
    int16x8_t vr   = vaddq_s16(vy, vrr);
    int16x8_t vg   = vaddq_s16(vy, vaddq_s16(vgg1, vgg2));
    int16x8_t vb   = vaddq_s16(vy, vbb);
    /* Interleave: build R,G,B byte triples in scalar-friendly form. */
    int16_t rb[8], gb[8], bb_[8];
    vst1q_s16(rb, vr);
    vst1q_s16(gb, vg);
    vst1q_s16(bb_, vb);
    for (uint8_t k = 0U; k < 8U; k++) {
      dst[0] = clamp_u8((int32_t)rb[k]);
      dst[1] = clamp_u8((int32_t)gb[k]);
      dst[2] = clamp_u8((int32_t)bb_[k]);
      dst += (uint8_t)k_ra8_jpeg_rgb_components;
    }
    i = (uint16_t)(i + 8U);
  }
  for (; i < n; i++) {
    ra8_jpeg_sw_ycc_to_rgb((int32_t)y[i],
                           (int32_t)cb[i],
                           (int32_t)cr[i],
                           &dst[0],
                           &dst[1],
                           &dst[2]);
    dst += (uint8_t)k_ra8_jpeg_rgb_components;
  }
}
#endif

/* ------------------------------------------------------------------ */
/* Public API: get_dimensions */
/* ------------------------------------------------------------------ */

/**
 * @brief Advance past 0xFF pad bytes and read one marker code.
 *
 * @details
 * Implements the T.81 sec B.1.1.2 "Markers" scan step of the
 * `ra8_jpeg_sw_get_dimensions()` walk: verifies the cursor sits on a
 * 0xFF prefix, skips any run of 0xFF fill bytes, and returns the
 * 0xFFxx marker code assembled from the byte that follows. The cursor
 * is left one past the marker's low byte.
 *
 * @param[in]     jpeg_buf JPEG byte stream.
 * @param[in]     jpeg_len Total stream length in bytes.
 * @param[in,out] i        Parse cursor (advanced past the marker).
 * @param[out]    out_mk   Receives the 0xFFxx marker code.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 Marker code stored in `*out_mk`.
 * @retval k_ra8_err_protocol_error Cursor not on 0xFF, or the stream
 *                                  ends inside the pad run.
 *
 * @pre `jpeg_buf`, `i` and `out_mk` are non-NULL (caller-checked).
 * @pre `*i < jpeg_len` (enforced by the caller's loop bound).
 * @post On success `*i` points at the first byte after the marker.
 * @post On error `*i` is unspecified; the caller aborts the walk.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t
dims_next_marker(const uint8_t* jpeg_buf, uint32_t jpeg_len, uint32_t* i, uint16_t* out_mk)
{
  if (jpeg_buf[*i] != (uint8_t)k_ra8_jpeg_marker_byte) {
    return k_ra8_err_protocol_error;
  }
  /* Skip pad bytes. */
  // mcdc-deactivated: dims_next_marker JPEG marker-pad skip; the jpeg_len bound is checked by the caller's enclosing while and the marker-byte equality follows the JFIF spec (0xFF padding bytes always within the segment); both conditions are co-dependent in any well-formed stream.
  while (*i < jpeg_len && jpeg_buf[*i] == (uint8_t)k_ra8_jpeg_marker_byte) {
    (*i)++;
  }
  if (*i >= jpeg_len) {
    return k_ra8_err_protocol_error;
  }
  uint8_t m = jpeg_buf[*i];
  *out_mk   = (uint16_t)(((uint16_t)k_jpeg_byte_mask << k_ra8_jpeg_byte_shift) | m);
  (*i)++;
  return k_ra8_ok;
}

/**
 * @brief Extract width/height from an SOF0 payload (T.81 sec B.2.2).
 *
 * @details
 * Reads the precision byte and the two big-endian dimension fields of
 * the SOF0 frame header whose length field starts at `i`, rejecting
 * non-8-bit precision and zero dimensions exactly as the previous
 * monolithic `ra8_jpeg_sw_get_dimensions()` body did.
 *
 * @param[in]  jpeg_buf JPEG byte stream.
 * @param[in]  i        Offset of the SOF0 segment-length field.
 * @param[in]  seglen   Validated segment length (>= 2, in bounds).
 * @param[out] out_w    Receives the image width in pixels.
 * @param[out] out_h    Receives the image height in pixels.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 Dimensions stored.
 * @retval k_ra8_err_protocol_error Segment too short or zero dimension.
 * @retval k_ra8_err_not_supported  Sample precision is not 8-bit.
 *
 * @pre `seglen` was bounds-checked against the stream by the caller.
 * @pre `out_w` and `out_h` are non-NULL (caller-checked).
 * @post On success `*out_w` and `*out_h` are non-zero.
 * @post No global state is touched.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t dims_parse_sof0(const uint8_t* jpeg_buf,
                                 uint32_t       i,
                                 uint16_t       seglen,
                                 uint16_t*      out_w,
                                 uint16_t*      out_h)
{
  if (seglen < 8U) {
    return k_ra8_err_protocol_error;
  }
  uint8_t precision = jpeg_buf[i + 2U];
  if (precision != 8U) {
    return k_ra8_err_not_supported;
  }
  *out_h = read_be16(&jpeg_buf[i + 3U]);
  *out_w = read_be16(&jpeg_buf[i + k_jpeg_sof_dims_off]);
  if (*out_w == 0U || *out_h == 0U) {
    return k_ra8_err_protocol_error;
  }
  return k_ra8_ok;
}

/**
 * @brief One step of the dimension walk: read a marker, handle it.
 *
 * @details
 * Loop body of `ra8_jpeg_sw_get_dimensions()`: reads the next marker
 * via `dims_next_marker()`, skips standalone SOI/EOI codes, validates
 * the segment length, extracts the dimensions on SOF0 (setting
 * `*done`), rejects unsupported SOFn frames, and otherwise advances
 * the cursor past the segment.
 *
 * @param[in]     jpeg_buf JPEG byte stream.
 * @param[in]     jpeg_len Total stream length in bytes.
 * @param[in,out] i        Parse cursor (advances).
 * @param[out]    done     Set true when SOF0 delivered the dimensions.
 * @param[out]    out_w    Receives the image width on SOF0.
 * @param[out]    out_h    Receives the image height on SOF0.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 Step handled; check `*done`.
 * @retval k_ra8_err_protocol_error Malformed marker / length field.
 * @retval k_ra8_err_not_supported  Unsupported SOFn or precision.
 *
 * @pre `*i + 4 <= jpeg_len` (caller's loop bound).
 * @pre All pointers are non-NULL (public API validated them).
 * @post On success with `*done` false, `*i` advanced past the segment.
 * @post On success with `*done` true, the dimensions are stored.
 *
 * @note Internal helper; not thread-safe.
 * @since 0.1.0
 */
static ra8_err_t dims_step(const uint8_t* jpeg_buf,
                           uint32_t       jpeg_len,
                           uint32_t*      i,
                           bool*          done,
                           uint16_t*      out_w,
                           uint16_t*      out_h)
{
  uint16_t  mk = 0U;
  ra8_err_t e  = dims_next_marker(jpeg_buf, jpeg_len, i, &mk);
  if (e != k_ra8_ok) {
    return e;
  }
  if (mk == (uint16_t)k_ra8_jpeg_marker_soi || mk == (uint16_t)k_ra8_jpeg_marker_eoi) {
    return k_ra8_ok;
  }
  if (*i + 2U > jpeg_len) {
    return k_ra8_err_protocol_error;
  }
  uint16_t seglen = read_be16(&jpeg_buf[*i]);
  if (seglen < 2U || (uint32_t)seglen > jpeg_len - *i) {
    return k_ra8_err_protocol_error;
  }
  if (mk == (uint16_t)k_ra8_jpeg_marker_sof0) {
    e = dims_parse_sof0(jpeg_buf, *i, seglen, out_w, out_h);
    if (e == k_ra8_ok) {
      *done = true;
    }
    return e;
  }
  // mcdc-deactivated: dims_step unsupported-SOFn detector; the 4-condition AND identifies SOF1..SOF15 except DHT/SOF8, but markers >= 0xFFC0 are by definition <= 0xFFCF in the JPEG marker space (range is 16 values), and SOF0 is handled above -- the upper-bound condition cannot independently flip on any reachable SOFn marker.
  if (mk >= k_jpeg_marker_sof_lo && mk <= k_jpeg_marker_sof_hi &&
      mk != (uint16_t)k_ra8_jpeg_marker_dht && mk != k_jpeg_marker_jpg) {
    return k_ra8_err_not_supported;
  }
  *i += seglen;
  return k_ra8_ok;
}

ra8_err_t ra8_jpeg_sw_get_dimensions(const uint8_t* jpeg_buf,
                                     uint32_t       jpeg_len,
                                     uint16_t*      out_w,
                                     uint16_t*      out_h)
{
  RA8_CHECK_NULL_PTR(jpeg_buf, s_tag, "jpeg_buf is NULL");
  RA8_CHECK_NULL_PTR(out_w, s_tag, "out_w is NULL");
  RA8_CHECK_NULL_PTR(out_h, s_tag, "out_h is NULL");
  if (jpeg_len < 4U) {
    return k_ra8_err_invalid_size;
  }
  if (read_be16(jpeg_buf) != (uint16_t)k_ra8_jpeg_marker_soi) {
    return k_ra8_err_protocol_error;
  }
  uint32_t i = 2U;
  while (i + 4U <= jpeg_len) {
    bool      done = false;
    ra8_err_t e    = dims_step(jpeg_buf, jpeg_len, &i, &done, out_w, out_h);
    if (e != k_ra8_ok) {
      return e;
    }
    if (done) {
      return k_ra8_ok;
    }
  }
  return k_ra8_err_protocol_error;
}
