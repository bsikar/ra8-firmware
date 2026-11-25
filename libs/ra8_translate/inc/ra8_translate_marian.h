/**
 * @file ra8_translate_marian.h
 * @brief C23 ABI for freestanding int8 Marian NMT container (`TMNT`) loading and
 * single-sequence greedy transformer inference.
 * @ingroup grp_translate
 *
 * @details
 * Reads quantized int8 weights directly in place from an external-memory `TMNT`
 * container without heap allocation, using ARMv8.1-M Helium MVE SIMD dot-product
 * kernels on Cortex-M85 and an exact scalar fallback on host builds.
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

/** @brief Wire constants and structural ceilings for the `TMNT` Marian NMT container. */
typedef enum : uint32_t {
  k_ra8_translate_marian_magic             = 0x544E4D54U, /**< LE "TMNT".           */
  k_ra8_translate_marian_version           = 1U,          /**< Format version.      */
  k_ra8_translate_marian_header_bytes      = 84U,         /**< Wire header bytes.   */
  k_ra8_translate_marian_tensor_desc_bytes = 24U,         /**< Descriptor size.     */
  k_ra8_translate_marian_max_layers        = 32U,         /**< Max enc/dec layers.  */
  k_ra8_translate_marian_max_d_model       = 2048U,       /**< Max hidden dim.      */
  k_ra8_translate_marian_max_d_ff          = 8192U,       /**< Max FFN inner dim.   */
  k_ra8_translate_marian_max_seq_len       = 2048U,       /**< Max sequence length. */
} ra8_translate_marian_const_t;

/** @brief Model normalization and weight-sharing bitmask flags in `TMNT` header. */
typedef enum : uint16_t {
  k_ra8_translate_marian_flag_none        = 0U,       /**< Post-norm default.   */
  k_ra8_translate_marian_flag_pre_norm    = 1U << 0U, /**< Pre-block LayerNorm. */
  k_ra8_translate_marian_flag_scale_embed = 1U << 1U, /**< Scale embed sqrt(d). */
  k_ra8_translate_marian_flag_tied_embed  = 1U << 2U, /**< Shares embed_w.      */
} ra8_translate_marian_model_flag_t;

static_assert(sizeof(ra8_translate_marian_model_flag_t) == 2U,
              "ra8_translate_marian_model_flag_t width");

/** @brief Open-time integrity verification bitmask for `ra8_translate_marian_open()`. */
typedef enum : uint8_t {
  k_ra8_translate_marian_open_meta_only      = 0U,       /**< Header + dir CRC-32. */
  k_ra8_translate_marian_open_verify_payload = 1U << 0U, /**< Full payload CRC-32. */
} ra8_translate_marian_open_flag_t;

static_assert(sizeof(ra8_translate_marian_open_flag_t) == 1U,
              "ra8_translate_marian_open_flag_t width");

/** @brief Quantization tensor element encoding inside `TMNT` descriptors. */
typedef enum : uint8_t {
  k_ra8_translate_marian_dtype_none = 0U, /**< Optional tensor absent.  */
  k_ra8_translate_marian_dtype_i8   = 1U, /**< Affine int8 weights.     */
  k_ra8_translate_marian_dtype_f32  = 2U, /**< IEEE-754 float32 vector. */
} ra8_translate_marian_dtype_t;

static_assert(sizeof(ra8_translate_marian_dtype_t) == 1U, "ra8_translate_marian_dtype_t width");

/**
 * @brief Validated metadata parsed from a serialized `TMNT` Marian NMT container.
 * @since 0.1.0
 */
typedef struct {
  uint32_t total_bytes;     /**< Total container byte length.                */
  uint32_t meta_crc32;      /**< CRC-32 IEEE over header and tensor table.   */
  uint32_t payload_crc32;   /**< CRC-32 IEEE over weights and vocab blob.    */
  uint32_t vocab_size;      /**< Model vocabulary size (>= 4).               */
  uint32_t unk_id;          /**< Unknown token identifier.                   */
  uint32_t bos_id;          /**< Decoder start / BOS token identifier.       */
  uint32_t eos_id;          /**< End-of-sequence token identifier.           */
  uint32_t pad_id;          /**< Padding token identifier.                   */
  uint32_t shortlist_off;   /**< Optional lexical shortlist offset (0=none). */
  uint32_t shortlist_bytes; /**< Optional lexical shortlist byte length.     */
  float    ln_eps;          /**< LayerNorm epsilon (> 0, finite).            */
  uint16_t version;         /**< Container format revision (== 1).           */
  uint16_t model_flags;     /**< `ra8_translate_marian_model_flag_t` mask.   */
  uint16_t d_model;         /**< Transformer hidden size.                    */
  uint16_t d_ff;            /**< Feed-forward inner dimension.               */
  uint16_t max_seq_len;     /**< Maximum supported source/target tokens.     */
  uint8_t  nmt_model;       /**< `ra8_translate_nmt_model_t` language pair.  */
  uint8_t  num_heads;       /**< Multi-head attention head count.            */
  uint8_t  num_enc_layers;  /**< Encoder stack depth.                        */
  uint8_t  num_dec_layers;  /**< Decoder stack depth.                        */
  uint8_t  reserved[6];     /**< Zero padding to 64-byte ABI boundary.       */
} ra8_translate_marian_header_t;

static_assert(sizeof(ra8_translate_marian_header_t) == 64U, "ra8_translate_marian_header_t size");
static_assert(alignof(ra8_translate_marian_header_t) == 4U, "ra8_translate_marian_header_t align");
static_assert(offsetof(ra8_translate_marian_header_t, total_bytes) == 0U,
              "marian hdr total_bytes off");
static_assert(offsetof(ra8_translate_marian_header_t, vocab_size) == 12U,
              "marian hdr vocab_size off");
static_assert(offsetof(ra8_translate_marian_header_t, ln_eps) == 40U, "marian hdr ln_eps off");
static_assert(offsetof(ra8_translate_marian_header_t, d_model) == 48U, "marian hdr d_model off");
static_assert(offsetof(ra8_translate_marian_header_t, nmt_model) == 54U,
              "marian hdr nmt_model off");
static_assert(offsetof(ra8_translate_marian_header_t, num_dec_layers) == 57U,
              "marian hdr dec_layers off");
static_assert(offsetof(ra8_translate_marian_header_t, reserved) == 58U, "marian hdr reserved off");
static_assert(sizeof(ra8_translate_marian_header_t) == 64U, "marian hdr tail");

/**
 * @brief Execution statistics reported by single-sequence greedy Marian decoding.
 * @since 0.1.0
 */
typedef struct {
  uint64_t weight_bytes_read; /**< Weight/bias bytes read. */
  uint32_t generated_tokens;  /**< Generated token count.  */
  uint8_t  stopped_on_eos;    /**< 1 if stopped at EOS.    */
  uint8_t  used_helium_mve;   /**< 1 if Helium MVE ran.    */
  uint8_t  reserved[2];       /**< Zero padding.           */
} ra8_translate_marian_stats_t;

static_assert(sizeof(ra8_translate_marian_stats_t) == 16U, "ra8_translate_marian_stats_t size");
static_assert(alignof(ra8_translate_marian_stats_t) == 8U, "ra8_translate_marian_stats_t align");
static_assert(offsetof(ra8_translate_marian_stats_t, weight_bytes_read) == 0U,
              "marian stats bytes off");
static_assert(offsetof(ra8_translate_marian_stats_t, generated_tokens) == 8U,
              "marian stats tokens off");
static_assert(offsetof(ra8_translate_marian_stats_t, stopped_on_eos) == 12U,
              "marian stats eos off");
static_assert(offsetof(ra8_translate_marian_stats_t, used_helium_mve) == 13U,
              "marian stats mve off");

/**
 * @brief Validate a serialized `TMNT` Marian NMT container and return parsed metadata.
 *
 * @details Calling context: `task-safe-reentrant`. Always checks `meta_crc32` over
 * the header and tensor descriptor directory; checks `payload_crc32` over weights
 * when `k_ra8_translate_marian_open_verify_payload` is set in @p open_flags.
 *
 * @param[in] tmnt_buf Valid serialized `TMNT` bytes in flash/external memory.
 * @param[in] tmnt_len Byte length of @p tmnt_buf.
 * @param[in] open_flags Bitmask of ::ra8_translate_marian_open_flag_t values.
 * @param[out] out_header Non-null destination receiving validated model metadata.
 * @return ::k_ra8_ok on success, or explicit error status with outputs unchanged.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_marian_open(const uint8_t*                 tmnt_buf,
                                    size_t                         tmnt_len,
                                    uint8_t                        open_flags,
                                    ra8_translate_marian_header_t* out_header);

/**
 * @brief Compute conservative scratch byte size for Marian single-sequence decode.
 *
 * @details Calling context: `task-safe-reentrant`. Sizes encoder buffers, cross-attention
 * KV cache, autoregressive self-attention KV cache, and per-step FFN/logit buffers.
 *
 * @param[in] tmnt_buf Validated `TMNT` container bytes.
 * @param[in] tmnt_len Byte length of @p tmnt_buf.
 * @param[in] max_src_len Maximum source token sequence length (`[1, max_seq_len]`).
 * @param[in] max_tgt_len Maximum target token generation steps (`[1, max_seq_len]`).
 * @param[out] out_work_bytes Non-null storage receiving required scratch byte count.
 * @return ::k_ra8_ok on success, or explicit error status with outputs unchanged.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_marian_work_bytes(const uint8_t* tmnt_buf,
                                          size_t         tmnt_len,
                                          uint16_t       max_src_len,
                                          uint16_t       max_tgt_len,
                                          size_t*        out_work_bytes);

/**
 * @brief Compute one decoder step's raw float32 logits for host/golden parity checks.
 *
 * @details Calling context: `task-safe-reentrant`. Encodes @p src_ids and advances
 * the decoder through @p prefix_ids (`prefix_len >= 1`, e.g. starting with `bos_id`)
 * to write exact final-step vocabulary logits `[vocab_size]` into @p out_logits.
 *
 * @param[in] tmnt_buf Validated `TMNT` container bytes.
 * @param[in] tmnt_len Byte length of @p tmnt_buf.
 * @param[in] src_ids Input SentencePiece token IDs (`[1, max_seq_len]`).
 * @param[in] src_len Element count of @p src_ids (> 0).
 * @param[in] prefix_ids Target decoder prefix token IDs (`prefix_len >= 1`).
 * @param[in] prefix_len Element count of @p prefix_ids (> 0).
 * @param[in,out] work_buf Non-null scratch workspace sized by ::ra8_translate_marian_work_bytes.
 * @param[in] work_cap Byte capacity of @p work_buf.
 * @param[out] out_logits Non-null destination receiving `vocab_size` step logits.
 * @param[in] logits_cap Element capacity of @p out_logits (`>= vocab_size`).
 * @return ::k_ra8_ok on success, or explicit error status with outputs unchanged.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_marian_step_logits(const uint8_t*  tmnt_buf,
                                           size_t          tmnt_len,
                                           const uint32_t* src_ids,
                                           size_t          src_len,
                                           const uint32_t* prefix_ids,
                                           size_t          prefix_len,
                                           uint8_t*        work_buf,
                                           size_t          work_cap,
                                           float*          out_logits,
                                           size_t          logits_cap);

/**
 * @brief Run single-sequence greedy encoder-decoder translation on token IDs.
 *
 * @details Calling context: `task-safe-reentrant`. Reads `TMNT` weights in place,
 * stops on `eos_id` (inclusive of generated `eos_id`) or `max_tgt_len`, and
 * leaves caller outputs untouched on any validation or capacity error.
 *
 * @param[in] tmnt_buf Validated `TMNT` container bytes.
 * @param[in] tmnt_len Byte length of @p tmnt_buf.
 * @param[in] src_ids Source SentencePiece token IDs (`>= 1`).
 * @param[in] src_len Element count of @p src_ids.
 * @param[in] max_tgt_len Maximum greedy target steps to generate (`[1, max_seq_len]`).
 * @param[in,out] work_buf Non-null scratch workspace sized by ::ra8_translate_marian_work_bytes.
 * @param[in] work_cap Byte capacity of @p work_buf.
 * @param[out] out_tgt_ids Caller array receiving generated target token IDs.
 * @param[in] tgt_cap Element capacity of @p out_tgt_ids (`>= max_tgt_len`).
 * @param[out] out_stats Non-null summary struct receiving token count and bytes read.
 * @return ::k_ra8_ok on success, or explicit error status with outputs unchanged.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_marian_greedy_decode(const uint8_t*                tmnt_buf,
                                             size_t                        tmnt_len,
                                             const uint32_t*               src_ids,
                                             size_t                        src_len,
                                             uint16_t                      max_tgt_len,
                                             uint8_t*                      work_buf,
                                             size_t                        work_cap,
                                             uint32_t*                     out_tgt_ids,
                                             size_t                        tgt_cap,
                                             ra8_translate_marian_stats_t* out_stats);

#ifdef __cplusplus
}
#endif
