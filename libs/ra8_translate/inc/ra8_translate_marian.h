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
  k_ra8_translate_marian_flag_none         = 0U,       /**< Post-norm default.   */
  k_ra8_translate_marian_flag_pre_norm     = 1U << 0U, /**< Pre-block LayerNorm. */
  k_ra8_translate_marian_flag_scale_embed  = 1U << 1U, /**< Scale embed sqrt(d). */
  k_ra8_translate_marian_flag_tied_embed   = 1U << 2U, /**< Shares embed_w.      */
  k_ra8_translate_marian_flag_decoder_ssru = 1U << 3U, /**< SSRU recurrent dec.  */
  k_ra8_translate_marian_flag_zero_bos     = 1U << 4U, /**< Zero step-0 embed.   */
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

/** @brief Wire constants for `TSLT` CSR lexical shortlists and batched Marian bounds. */
typedef enum : uint32_t {
  k_ra8_translate_marian_shortlist_magic        = 0x544C5354U, /**< LE "TSLT".        */
  k_ra8_translate_marian_shortlist_version      = 1U,          /**< Format version.   */
  k_ra8_translate_marian_shortlist_header_bytes = 16U,         /**< CSR header bytes. */
  k_ra8_translate_marian_max_batch              = 16U,         /**< Max batch width.  */
  k_ra8_translate_marian_max_beam               = 4U,          /**< Max beam width.   */
} ra8_translate_marian_batch_const_t;

/** @brief Execution mode bitmask for batched Marian decoding. */
typedef enum : uint16_t {
  k_ra8_translate_marian_batch_flag_none          = 0U,       /**< Full-vocab greedy.    */
  k_ra8_translate_marian_batch_flag_use_shortlist = 1U << 0U, /**< Enable `TSLT` prune.  */
  k_ra8_translate_marian_batch_flag_beam_search   = 1U << 1U, /**< Enable beam (`2..4`). */
} ra8_translate_marian_batch_flag_t;

static_assert(sizeof(ra8_translate_marian_batch_flag_t) == 2U,
              "ra8_translate_marian_batch_flag_t width");

/**
 * @brief Execution configuration for batched Marian NMT queue decoding.
 * @since 0.1.0
 */
typedef struct {
  uint16_t max_src_len; /**< Maximum source tokens per bubble (`[1, max_seq_len]`).    */
  uint16_t max_tgt_len; /**< Maximum generated target tokens per bubble (`>= 1`).      */
  uint16_t flags;       /**< `ra8_translate_marian_batch_flag_t` bitmask.              */
  uint8_t  batch_size;  /**< Max concurrent bubbles in flight (`[1, 16]`).             */
  uint8_t  beam_width;  /**< Beam width (`1` if beam flag clear, `[2, 4]` if enabled). */
  uint8_t  reserved[4]; /**< Zero padding.                                             */
} ra8_translate_marian_batch_cfg_t;

static_assert(sizeof(ra8_translate_marian_batch_cfg_t) == 12U, "marian batch cfg size");
static_assert(alignof(ra8_translate_marian_batch_cfg_t) == 2U, "marian batch cfg align");
static_assert(offsetof(ra8_translate_marian_batch_cfg_t, max_tgt_len) == 2U, "batch cfg tgt");
static_assert(offsetof(ra8_translate_marian_batch_cfg_t, flags) == 4U, "batch cfg flags");
static_assert(offsetof(ra8_translate_marian_batch_cfg_t, batch_size) == 6U, "batch cfg batch");
static_assert(offsetof(ra8_translate_marian_batch_cfg_t, beam_width) == 7U, "batch cfg beam");
static_assert(offsetof(ra8_translate_marian_batch_cfg_t, reserved) == 8U, "batch cfg rsv");

/**
 * @brief Flat-pool queue descriptor for one source bubble in batched decoding.
 * @since 0.1.0
 */
typedef struct {
  uint32_t src_off;        /**< Start index in `src_pool`.               */
  uint32_t src_len;        /**< Source token count (`[1, max_src_len]`). */
  uint32_t tgt_off;        /**< Start index in `out_tgt_pool`.           */
  uint32_t tgt_cap;        /**< Writable token capacity (`>= 1`).        */
  uint32_t out_len;        /**< Output tokens written on completion.     */
  uint8_t  stopped_on_eos; /**< 1 if terminated on `eos_id`, else 0.     */
  uint8_t  completed;      /**< 1 once decoded and written, else 0.      */
  uint8_t  reserved[2];    /**< Zero padding.                            */
} ra8_translate_marian_batch_job_t;

static_assert(sizeof(ra8_translate_marian_batch_job_t) == 24U, "marian batch job size");
static_assert(alignof(ra8_translate_marian_batch_job_t) == 4U, "marian batch job align");
static_assert(offsetof(ra8_translate_marian_batch_job_t, src_len) == 4U, "batch job srclen");
static_assert(offsetof(ra8_translate_marian_batch_job_t, tgt_off) == 8U, "batch job tgtoff");
static_assert(offsetof(ra8_translate_marian_batch_job_t, tgt_cap) == 12U, "batch job tgtcap");
static_assert(offsetof(ra8_translate_marian_batch_job_t, out_len) == 16U, "batch job outlen");
static_assert(offsetof(ra8_translate_marian_batch_job_t, stopped_on_eos) == 20U, "batch job eos");
static_assert(offsetof(ra8_translate_marian_batch_job_t, completed) == 21U, "batch job done");

/**
 * @brief Summary counters emitted after draining a batched Marian bubble queue.
 * @since 0.1.0
 */
typedef struct {
  uint64_t total_weight_bytes_read; /**< Cumulative weight/bias/shortlist bytes. */
  uint32_t completed_jobs;          /**< Total chapter queue jobs decoded.       */
  uint32_t decoder_steps;           /**< Number of batched decoder steps run.    */
  uint32_t total_generated_tokens;  /**< Sum of generated tokens across jobs.    */
  uint8_t  used_helium_mve;         /**< 1 if Helium MVE kernel executed.        */
  uint8_t  reserved[3];             /**< Zero padding.                           */
} ra8_translate_marian_batch_stats_t;

static_assert(sizeof(ra8_translate_marian_batch_stats_t) == 24U, "marian batch stats size");
static_assert(alignof(ra8_translate_marian_batch_stats_t) == 8U, "marian batch stats align");
static_assert(offsetof(ra8_translate_marian_batch_stats_t, completed_jobs) == 8U, "stats jobs");
static_assert(offsetof(ra8_translate_marian_batch_stats_t, decoder_steps) == 12U, "stats steps");
static_assert(offsetof(ra8_translate_marian_batch_stats_t, total_generated_tokens) == 16U,
              "stats tokens");
static_assert(offsetof(ra8_translate_marian_batch_stats_t, used_helium_mve) == 20U, "stats mve");

/**
 * @brief Compute conservative scratch workspace bytes for batched Marian decode.
 *
 * @details Calling context: `task-safe-reentrant`. Sizes `batch_size * beam_width`
 * self-attention KV caches, `batch_size` encoder/cross-KV buffers, active shortlist
 * sets, and per-step batched activation scratch inside caller memory.
 *
 * @param[in] tmnt_buf Validated `TMNT` container bytes.
 * @param[in] tmnt_len Byte length of @p tmnt_buf.
 * @param[in] cfg Validated ::ra8_translate_marian_batch_cfg_t configuration.
 * @param[out] out_work_bytes Non-null destination receiving required workspace bytes.
 * @return ::k_ra8_ok on success, or explicit error status with outputs unchanged.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_marian_batch_work_bytes(const uint8_t*                          tmnt_buf,
                                                size_t                                  tmnt_len,
                                                const ra8_translate_marian_batch_cfg_t* cfg,
                                                size_t* out_work_bytes);

/**
 * @brief Drain a bubble queue using shared-weight batched decoding and optional shortlist/beam.
 *
 * @details Calling context: `task-safe-reentrant`. Refills completed slots
 * immediately from @p jobs while other sequences remain in flight, streams each
 * decoder weight tensor once per step across active beams, optionally prunes the
 * output projection via `TSLT` lexical shortlists, and logs per-step weight bytes
 * when @p out_step_bytes is provided. Leaves all caller outputs untouched on any error.
 *
 * @param[in] tmnt_buf Validated `TMNT` container bytes.
 * @param[in] tmnt_len Byte length of @p tmnt_buf.
 * @param[in] cfg Batch/shortlist/beam configuration.
 * @param[in] src_pool Concatenated source token IDs indexed by ::ra8_translate_marian_batch_job_t.
 * @param[in] src_pool_len Element count of @p src_pool.
 * @param[in,out] jobs Non-null queue descriptors (`job_count >= 1`).
 * @param[in] job_count Number of jobs in @p jobs.
 * @param[in,out] work_buf Non-null scratch buffer sized by ::ra8_translate_marian_batch_work_bytes.
 * @param[in] work_cap Byte capacity of @p work_buf.
 * @param[out] out_tgt_pool Destination pool receiving decoded token IDs per job offset.
 * @param[in] tgt_pool_cap Element capacity of @p out_tgt_pool.
 * @param[out] out_step_bytes Optional per-step weight-bytes log (NULL allowed only if @p step_bytes_cap == 0).
 * @param[in] step_bytes_cap Element capacity of @p out_step_bytes (`>= sum(min(max_tgt_len, tgt_cap))` when non-null).
 * @param[out] out_stats Non-null summary struct receiving cumulative bytes and step counts.
 * @return ::k_ra8_ok on success, or explicit error status with outputs unchanged.
 * @since 0.1.0
 */
ra8_err_t ra8_translate_marian_batch_decode(const uint8_t*                          tmnt_buf,
                                            size_t                                  tmnt_len,
                                            const ra8_translate_marian_batch_cfg_t* cfg,
                                            const uint32_t*                         src_pool,
                                            size_t                                  src_pool_len,
                                            ra8_translate_marian_batch_job_t*       jobs,
                                            size_t                                  job_count,
                                            uint8_t*                                work_buf,
                                            size_t                                  work_cap,
                                            uint32_t*                               out_tgt_pool,
                                            size_t                                  tgt_pool_cap,
                                            uint64_t*                               out_step_bytes,
                                            size_t                                  step_bytes_cap,
                                            ra8_translate_marian_batch_stats_t*     out_stats);

#ifdef __cplusplus
}
#endif
