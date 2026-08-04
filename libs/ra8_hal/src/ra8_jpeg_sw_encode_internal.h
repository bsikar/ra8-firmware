/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_jpeg_sw_encode_internal.h
 * @brief Module-private declarations shared by the two encoder
 *        translation units of the software JPEG codec.
 * @ingroup grp_hal_camera
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The pure-software baseline JPEG encoder is split across two
 * translation units to keep each file under the maintainability
 * line cap:
 *
 *   - `ra8_jpeg_sw_encode.c`      -- pixel pipeline: RGB->YCbCr strip
 *                                   conversion, 4:2:0 sampling, forward
 *                                   DCT, quantization, per-block Huffman
 *                                   emission and the `ra8_jpeg_sw_encode()`
 *                                   public API.
 *   - `ra8_jpeg_sw_encode_emit.c` -- byte/bit emitters, the T.81 Annex
 *                                   K.3.3 reference Huffman tables, the
 *                                   canonical code build and the JFIF
 *                                   header-segment writers.
 *
 * Every symbol referenced by both units lives here: the encoder
 * context struct and the `ra8_jpeg_sw_priv_enc_*` primitives the
 * pixel pipeline calls into.
 *
 * Spec citations are tagged `T.81 sec X.Y "..."` and refer to
 * ITU-T Recommendation T.81 (1992) | ISO/IEC 10918-1.
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_jpeg_sw_internal.h"

/**
 * @struct ra8_jpeg_enc_ctx_t
 * @brief Encoder state -- buffer cursor, bit accumulator, scaled
 *        quant tables, Huffman LUTs and DC predictors.
 *
 * @details
 * One instance drives a whole `ra8_jpeg_sw_encode()` call: the
 * destination byte cursor and MSB-first bit accumulator feed the
 * entropy emitters in `ra8_jpeg_sw_encode_emit.c`, while the scaled
 * quantization tables, canonical Huffman code/size look-up tables and
 * per-component DC predictors are consumed by the block encoder in
 * `ra8_jpeg_sw_encode.c`.
 *
 * @invariant `pos <= cap` whenever `overflow` is false.
 * @invariant `bit_cnt < 8` between `ra8_jpeg_sw_priv_enc_put_bits()` calls.
 *
 * @see ra8_jpeg_sw_encode()  Public API that owns the single instance.
 * @since 0.1.0
 */
typedef struct {
  uint8_t* dst; /**< Destination JPEG byte buffer. */
  uint32_t cap; /**< Capacity of `dst` in bytes.   */
  uint32_t pos; /**< Write cursor into `dst`.      */

  uint32_t bit_buf; /**< MSB-first entropy bit accumulator. */
  uint8_t  bit_cnt; /**< Bits currently valid in `bit_buf`. */

  uint8_t qy[k_ra8_jpeg_block_size]; /**< Scaled luma quant table.   */
  uint8_t qc[k_ra8_jpeg_block_size]; /**< Scaled chroma quant table. */

  int32_t dc_pred[k_ra8_jpeg_max_comps]; /**< Per-component DC predictors. */

  uint16_t code_dc_l[k_ra8_jpeg_huff_max]; /**< Luma DC Huffman codes.      */
  uint8_t  size_dc_l[k_ra8_jpeg_huff_max]; /**< Luma DC code lengths.       */
  uint16_t code_ac_l[k_ra8_jpeg_huff_max]; /**< Luma AC Huffman codes.      */
  uint8_t  size_ac_l[k_ra8_jpeg_huff_max]; /**< Luma AC code lengths.       */
  uint16_t code_dc_c[k_ra8_jpeg_huff_max]; /**< Chroma DC Huffman codes.    */
  uint8_t  size_dc_c[k_ra8_jpeg_huff_max]; /**< Chroma DC code lengths.     */
  uint16_t code_ac_c[k_ra8_jpeg_huff_max]; /**< Chroma AC Huffman codes.    */
  uint8_t  size_ac_c[k_ra8_jpeg_huff_max]; /**< Chroma AC code lengths.     */
  bool     overflow;                       /**< Set on capacity exhaustion. */
} ra8_jpeg_enc_ctx_t;

/**
 * @brief Append a 16-bit big-endian word to the JPEG byte stream.
 *
 * @details Emits the high byte then the low byte through the internal
 *          byte emitter; sets `e->overflow` instead of writing past
 *          `e->cap`. Defined in `ra8_jpeg_sw_encode_emit.c`.
 *
 * @param[in,out] e Encoder context (cursor advances).
 * @param[in]     v Value to append (marker code or length field).
 *
 * @pre `e` is non-NULL with `dst` bound (module-internal call chain).
 * @pre `e->pos <= e->cap`.
 * @post `e->pos` advanced by 2, or `e->overflow` set.
 * @post No other context state is mutated.
 *
 * @note Internal primitive; not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV void ra8_jpeg_sw_priv_enc_emit_u16(ra8_jpeg_enc_ctx_t* e, uint16_t v);

/**
 * @brief Push `n` bits MSB-first into the entropy stream with stuffing.
 *
 * @details Accumulates bits and flushes whole bytes, inserting the
 *          T.81 sec F.1.2.3 `0x00` stuffing byte after every emitted
 *          `0xFF`. Defined in `ra8_jpeg_sw_encode_emit.c`.
 *
 * @param[in,out] e    Encoder context (bit buffer + cursor mutate).
 * @param[in]     code Right-aligned code bits to append.
 * @param[in]     n    Number of significant bits in `code` (0..24).
 *
 * @pre `e->bit_cnt + n` fits the 32-bit accumulator (n <= 24).
 * @pre `e` is non-NULL (module-internal call chain).
 * @post `e->bit_cnt < 8` (all whole bytes flushed).
 * @post `e->overflow` set instead of any out-of-bounds write.
 *
 * @note Internal primitive; not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV void ra8_jpeg_sw_priv_enc_put_bits(ra8_jpeg_enc_ctx_t* e, uint32_t code, uint8_t n);

/**
 * @brief Flush any partial byte at the end of a scan with 1-fill.
 *
 * @details Pads the residual `bit_cnt` bits with ones (T.81 sec
 *          F.1.2.3) so the final entropy byte is complete before the
 *          EOI marker. Defined in `ra8_jpeg_sw_encode_emit.c`.
 *
 * @param[in,out] e Encoder context (bit buffer + cursor mutate).
 *
 * @pre `e` is non-NULL (module-internal call chain).
 * @pre All scan data was emitted via `ra8_jpeg_sw_priv_enc_put_bits()`.
 * @post `e->bit_cnt == 0`.
 * @post The last entropy byte, if partial, is 1-padded in `dst`.
 *
 * @note Internal primitive; not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV void ra8_jpeg_sw_priv_enc_flush_bits(ra8_jpeg_enc_ctx_t* e);

/**
 * @brief Build the four Annex K.3.3 Huffman code/size LUTs.
 *
 * @details Canonical-builds the luma/chroma DC/AC code and size
 *          look-up tables into the encoder context and returns each
 *          table's symbol count for the DHT emitters. Defined in
 *          `ra8_jpeg_sw_encode_emit.c`.
 *
 * @param[in,out] e          Encoder context (code/size LUTs written).
 * @param[out]    total_dc_l Luma DC symbol count.
 * @param[out]    total_ac_l Luma AC symbol count.
 * @param[out]    total_dc_c Chroma DC symbol count.
 * @param[out]    total_ac_c Chroma AC symbol count.
 *
 * @pre All five pointers are non-NULL (module-internal call chain).
 * @pre `e`'s LUT arrays are writable.
 * @post All four LUT pairs hold canonical Huffman codes.
 * @post The totals reflect the static K.3.3 BITS arrays.
 *
 * @note Internal primitive; not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV void ra8_jpeg_sw_priv_enc_build_huff(ra8_jpeg_enc_ctx_t* e,
                                              uint16_t*           total_dc_l,
                                              uint16_t*           total_ac_l,
                                              uint16_t*           total_dc_c,
                                              uint16_t*           total_ac_c);

/**
 * @brief Emit the SOI/APP0/DQT/SOF0/DHT/SOS header segments.
 *
 * @details Writes every byte that precedes the entropy-coded scan of a
 *          4:2:0 YCbCr baseline stream, in JFIF 1.1 order. Defined in
 *          `ra8_jpeg_sw_encode_emit.c`.
 *
 * @param[in,out] e          Encoder context (cursor advances).
 * @param[in]     w          Image width in pixels.
 * @param[in]     h          Image height in pixels.
 * @param[in]     total_dc_l Luma DC symbol count (from build_huff).
 * @param[in]     total_ac_l Luma AC symbol count.
 * @param[in]     total_dc_c Chroma DC symbol count.
 * @param[in]     total_ac_c Chroma AC symbol count.
 *
 * @pre `ra8_jpeg_sw_priv_enc_build_huff()` populated the totals.
 * @pre `e->qy`/`e->qc` hold the scaled quantization tables.
 * @post `e->pos` advanced past every header segment.
 * @post Header bytes are byte-identical to the pre-split encoder.
 *
 * @note Internal primitive; not thread-safe.
 * @since 0.1.0
 */
RA8_PRIV void ra8_jpeg_sw_priv_enc_headers(ra8_jpeg_enc_ctx_t* e,
                                           uint16_t            w,
                                           uint16_t            h,
                                           uint16_t            total_dc_l,
                                           uint16_t            total_ac_l,
                                           uint16_t            total_dc_c,
                                           uint16_t            total_ac_c);
