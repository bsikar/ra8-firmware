/**
 * @file ra8_translate_ctc.h
 * @brief C23 ABI for per-script CTC character tables (`TCHR`) and greedy OCR decoding.
 * @ingroup grp_translate
 *
 * @details
 * Exposes validated binary character tables (`TCHR`) produced by the offline
 * Korean, Japanese, and Chinese OCR recognizer pipelines alongside zero-allocation
 * CTC greedy decoding for quantized `int8_t` Ethos-U55 outputs and `float` host
 * logits, producing UTF-8 line text and basis-point confidence scores compatible
 * with `TRN1` sidecar records.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Wire constants and confidence bounds for `TCHR` CTC tables and decoding. */
typedef enum : uint32_t {
  k_ra8_translate_ctc_magic                = 0x52484354U, /**< Little-endian "TCHR".          */
  k_ra8_translate_ctc_version              = 1U,          /**< Supported format revision.     */
  k_ra8_translate_ctc_header_bytes         = 36U,         /**< Fixed wire `TCHR` header size. */
  k_ra8_translate_ctc_max_classes          = 65536U,      /**< Max classes incl. CTC blank.   */
  k_ra8_translate_ctc_max_glyph_bytes      = 64U,         /**< Max UTF-8 bytes per class.     */
  k_ra8_translate_ctc_index_none           = 0xFFFFFFFFU, /**< Sentinel for absent unk class. */
  k_ra8_translate_ctc_max_conf_bps         = 10000U,      /**< 100.00% confidence basis pts.  */
  k_ra8_translate_ctc_default_low_conf_bps = 7500U,       /**< Default low-conf flag cutoff.  */
} ra8_translate_ctc_const_t;

/** @brief Input tensor numeric interpretation for CTC greedy decoding. */
typedef enum : uint8_t {
  k_ra8_translate_ctc_score_logits = 0U, /**< Unnormalized logits (apply softmax).    */
  k_ra8_translate_ctc_score_probs  = 1U, /**< Pre-normalized probabilities in [0, 1]. */
} ra8_translate_ctc_score_mode_t;

static_assert(sizeof(ra8_translate_ctc_score_mode_t) == 1U, "ra8_translate_ctc_score_mode_t width");

/** @brief Bitmask flags controlling CTC emission behavior. */
typedef enum : uint8_t {
  k_ra8_translate_ctc_flag_none     = 0U,       /**< Emit table glyph for every non-blank.   */
  k_ra8_translate_ctc_flag_skip_unk = 1U << 0U, /**< Omit UTF-8 bytes when argmax is unk_id. */
} ra8_translate_ctc_flag_t;

static_assert(sizeof(ra8_translate_ctc_flag_t) == 1U, "ra8_translate_ctc_flag_t width");

/**
 * @brief Validated metadata summary parsed from a serialized `TCHR` character table.
 * @since 0.1.0
 */
typedef struct {
  uint32_t total_bytes;       /**< Total validated `TCHR` byte length.                  */
  uint32_t container_crc32;   /**< CRC-32 IEEE recorded in container header.            */
  uint16_t version;           /**< Container format revision (== 1).                    */
  uint16_t flags;             /**< Reserved format flags (== 0).                        */
  uint32_t class_count;       /**< Total classes including CTC blank.                   */
  uint32_t blank_index;       /**< Class ID treated as CTC blank.                       */
  uint32_t unk_index;         /**< Unknown token class ID or index_none.                */
  uint32_t string_pool_bytes; /**< UTF-8 string pool length in bytes.                   */
  uint8_t  ocr_model;         /**< Script profile matching `ra8_translate_ocr_model_t`. */
  uint8_t  reserved[3];       /**< Zero padding.                                        */
} ra8_translate_ctc_charset_header_t;

static_assert(sizeof(ra8_translate_ctc_charset_header_t) == 32U,
              "ra8_translate_ctc_charset_header_t size");
static_assert(alignof(ra8_translate_ctc_charset_header_t) == 4U,
              "ra8_translate_ctc_charset_header_t align");
static_assert(offsetof(ra8_translate_ctc_charset_header_t, total_bytes) == 0U,
              "ctc header total_bytes off");
static_assert(offsetof(ra8_translate_ctc_charset_header_t, container_crc32) == 4U,
              "ctc header crc off");
static_assert(offsetof(ra8_translate_ctc_charset_header_t, version) == 8U, "ctc header ver off");
static_assert(offsetof(ra8_translate_ctc_charset_header_t, flags) == 10U, "ctc header flags off");
static_assert(offsetof(ra8_translate_ctc_charset_header_t, class_count) == 12U,
              "ctc header class_count off");
static_assert(offsetof(ra8_translate_ctc_charset_header_t, blank_index) == 16U,
              "ctc header blank_index off");
static_assert(offsetof(ra8_translate_ctc_charset_header_t, unk_index) == 20U,
              "ctc header unk_index off");
static_assert(offsetof(ra8_translate_ctc_charset_header_t, string_pool_bytes) == 24U,
              "ctc header pool_bytes off");
static_assert(offsetof(ra8_translate_ctc_charset_header_t, ocr_model) == 28U,
              "ctc header ocr_model off");
static_assert(offsetof(ra8_translate_ctc_charset_header_t, reserved) == 29U,
              "ctc header reserved off");

/**
 * @brief Decoding geometry, score interpretation, and low-confidence threshold.
 * @since 0.1.0
 */
typedef struct {
  uint32_t                       time_steps;                /**< Number of CTC time frames (> 0). */
  uint32_t                       class_stride;              /**< Frame stride (0 = packed C).     */
  ra8_translate_ctc_score_mode_t score_mode;                /**< Logits vs probability mode.      */
  uint8_t                        decode_flags;              /**< `ra8_translate_ctc_flag_t` mask. */
  uint16_t                       low_confidence_thresh_bps; /**< Flag cutoff [0, 10000].          */
  uint8_t                        expected_ocr_model;        /**< OCR model ID or 0.               */
  uint8_t                        reserved[3];               /**< Reserved padding; zero.          */
} ra8_translate_ctc_decode_cfg_t;

static_assert(sizeof(ra8_translate_ctc_decode_cfg_t) == 16U, "ra8_translate_ctc_decode_cfg_t size");
static_assert(alignof(ra8_translate_ctc_decode_cfg_t) == 4U,
              "ra8_translate_ctc_decode_cfg_t align");
static_assert(offsetof(ra8_translate_ctc_decode_cfg_t, time_steps) == 0U, "ctc cfg time_steps off");
static_assert(offsetof(ra8_translate_ctc_decode_cfg_t, class_stride) == 4U,
              "ctc cfg class_stride off");
static_assert(offsetof(ra8_translate_ctc_decode_cfg_t, score_mode) == 8U, "ctc cfg score_mode off");
static_assert(offsetof(ra8_translate_ctc_decode_cfg_t, decode_flags) == 9U,
              "ctc cfg decode_flags off");
static_assert(offsetof(ra8_translate_ctc_decode_cfg_t, low_confidence_thresh_bps) == 10U,
              "ctc cfg thresh off");
static_assert(offsetof(ra8_translate_ctc_decode_cfg_t, expected_ocr_model) == 12U,
              "ctc cfg ocr_model off");
static_assert(offsetof(ra8_translate_ctc_decode_cfg_t, reserved) == 13U, "ctc cfg reserved off");

/**
 * @brief Decoded line metrics returned alongside UTF-8 text.
 * @since 0.1.0
 */
typedef struct {
  uint32_t text_bytes;               /**< UTF-8 bytes written into `out_utf8`.       */
  uint32_t emitted_tokens;           /**< Non-blank character runs emitted.          */
  uint32_t blank_frames;             /**< Frames whose argmax equaled `blank_index`. */
  uint32_t unk_count;                /**< Emitted tokens equal to `unk_index`.       */
  uint16_t confidence_bps;           /**< Mean peak char confidence in [0, 10000].   */
  uint16_t min_token_confidence_bps; /**< Minimum peak token confidence [0, 10000].  */
  uint8_t  low_confidence;           /**< 1 if below threshold or empty line else 0. */
  uint8_t  reserved[3];              /**< Zero padding.                              */
} ra8_translate_ctc_result_t;

static_assert(sizeof(ra8_translate_ctc_result_t) == 24U, "ra8_translate_ctc_result_t size");
static_assert(alignof(ra8_translate_ctc_result_t) == 4U, "ra8_translate_ctc_result_t align");
static_assert(offsetof(ra8_translate_ctc_result_t, text_bytes) == 0U, "ctc res text_bytes off");
static_assert(offsetof(ra8_translate_ctc_result_t, emitted_tokens) == 4U, "ctc res emitted off");
static_assert(offsetof(ra8_translate_ctc_result_t, blank_frames) == 8U, "ctc res blank off");
static_assert(offsetof(ra8_translate_ctc_result_t, unk_count) == 12U, "ctc res unk off");
static_assert(offsetof(ra8_translate_ctc_result_t, confidence_bps) == 16U, "ctc res conf off");
static_assert(offsetof(ra8_translate_ctc_result_t, min_token_confidence_bps) == 18U,
              "ctc res min_conf off");
static_assert(offsetof(ra8_translate_ctc_result_t, low_confidence) == 20U, "ctc res low_conf off");
static_assert(offsetof(ra8_translate_ctc_result_t, reserved) == 21U, "ctc res reserved off");

/**
 * @brief Validate a serialized `TCHR` CTC character table container.
 *
 * @details Calling context: `task-safe-reentrant`. Verifies magic, revision,
 * script profile, class/blank/unknown indices, monotonic UTF-8 glyph offsets,
 * and CRC-32 checksum before writing @p out_header.
 *
 * @param[in] tchr_buf Serialized `TCHR` container bytes.
 * @param[in] tchr_len Byte length of @p tchr_buf.
 * @param[out] out_header Non-null destination for validated metadata.
 * @return ::k_ra8_ok on success, or explicit error status with outputs unchanged.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_ctc_charset_open(const uint8_t*                      tchr_buf,
                                         size_t                              tchr_len,
                                         ra8_translate_ctc_charset_header_t* out_header);

/**
 * @brief Borrow the UTF-8 glyph string for one CTC class index.
 *
 * @details Calling context: `task-safe-reentrant`. Returned @p out_utf8_ptr
 * aliases inside @p tchr_buf while @p tchr_buf stays valid.
 *
 * @param[in] tchr_buf Validated `TCHR` container bytes.
 * @param[in] tchr_len Byte length of @p tchr_buf.
 * @param[in] class_index Class index in `[0, class_count)`.
 * @param[out] out_utf8_ptr Non-null out-pointer receiving UTF-8 glyph base.
 * @param[out] out_utf8_len Non-null storage receiving UTF-8 byte length.
 * @return ::k_ra8_ok on success, or explicit error status with outputs unchanged.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_ctc_charset_glyph(const uint8_t*  tchr_buf,
                                          size_t          tchr_len,
                                          uint32_t        class_index,
                                          const uint8_t** out_utf8_ptr,
                                          uint16_t*       out_utf8_len);

/**
 * @brief Greedy CTC decode quantized `int8_t` recognizer output `[T, C]` to UTF-8.
 *
 * @details Calling context: `task-safe-reentrant`. Collapses blank-separated CTC
 * repeats with zero heap allocation, scores per-character peak confidence using
 * `(q - zero_point) * quant_scale`, and leaves caller buffers untouched on error.
 *
 * @param[in] tchr_buf Validated `TCHR` character table matching recognizer classes.
 * @param[in] tchr_len Byte length of @p tchr_buf.
 * @param[in] logits_i8 Quantized `[T, C]` activations with optional row stride.
 * @param[in] logit_elem_count Readable element count of @p logits_i8.
 * @param[in] quant_scale Positive finite affine scale factor.
 * @param[in] zero_point Affine zero point `[-128, 127]`.
 * @param[in] cfg Validated sequence geometry and confidence threshold configuration.
 * @param[out] out_utf8 Non-null destination buffer for decoded UTF-8 line text.
 * @param[in] out_cap Byte capacity of @p out_utf8.
 * @param[out] out_result Non-null summary struct receiving text length and confidence.
 * @return ::k_ra8_ok on success, or explicit error status with outputs unchanged.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_ctc_decode_i8(const uint8_t*                        tchr_buf,
                                      size_t                                tchr_len,
                                      const int8_t*                         logits_i8,
                                      size_t                                logit_elem_count,
                                      float                                 quant_scale,
                                      int8_t                                zero_point,
                                      const ra8_translate_ctc_decode_cfg_t* cfg,
                                      uint8_t*                              out_utf8,
                                      size_t                                out_cap,
                                      ra8_translate_ctc_result_t*           out_result);

/**
 * @brief Greedy CTC decode floating-point recognizer output `[T, C]` to UTF-8.
 *
 * @details Calling context: `task-safe-reentrant`. Executes the same blank collapse
 * and peak confidence scoring as ::ra8_translate_ctc_decode_i8 directly on host or
 * dequantized `float` logits.
 *
 * @param[in] tchr_buf Validated `TCHR` character table matching recognizer classes.
 * @param[in] tchr_len Byte length of @p tchr_buf.
 * @param[in] logits_f32 Finite `[T, C]` float logits or probabilities.
 * @param[in] logit_elem_count Readable element count of @p logits_f32.
 * @param[in] cfg Validated sequence geometry and confidence threshold configuration.
 * @param[out] out_utf8 Non-null destination buffer for decoded UTF-8 line text.
 * @param[in] out_cap Byte capacity of @p out_utf8.
 * @param[out] out_result Non-null summary struct receiving text length and confidence.
 * @return ::k_ra8_ok on success, or explicit error status with outputs unchanged.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_ctc_decode_f32(const uint8_t*                        tchr_buf,
                                       size_t                                tchr_len,
                                       const float*                          logits_f32,
                                       size_t                                logit_elem_count,
                                       const ra8_translate_ctc_decode_cfg_t* cfg,
                                       uint8_t*                              out_utf8,
                                       size_t                                out_cap,
                                       ra8_translate_ctc_result_t*           out_result);

#ifdef __cplusplus
}
#endif
