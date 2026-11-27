/**
 * @file translate_e2e.h
 * @brief Target and host-shared E2E runner for `ra8_translate` chapter jobs and `TPAK` packs.
 *
 * @details
 * Provides caller-allocated arenas and deterministic binary fixture builders
 * exercising the full translation pipeline (`det_post_i8` -> `ctc_decode_i8` ->
 * `TGL1` glossary -> `tok_encode` -> `marian_greedy_decode` -> `tok_decode` ->
 * `overlay_fit_text` -> `TRJC` resume -> `TRN1`) with zero dynamic allocation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_translate.h"
#include "ra8_translate_job.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Fixed static arena capacities sized for SDRAM and host unit tests. */
typedef enum : uint32_t {
  k_te2e_job_work_cap    = 262144U, /**< Job workspace capacity (`>= job_work_bytes`).    */
  k_te2e_model_work_cap  = 262144U, /**< Shared detector / tok / Marian scratch arena.    */
  k_te2e_ckpt_cap        = 65536U,  /**< `TRJC` mid-chapter checkpoint snapshot buffer.   */
  k_te2e_trn1_cap        = 32768U,  /**< Serialized `TRN1` sidecar output capacity.       */
  k_te2e_tgl1_cap        = 4096U,   /**< Serialized `TGL1` glossary buffer capacity.      */
  k_te2e_det_map_cap     = 8192U,   /**< Per-page quantized `int8_t` detector map buffer. */
  k_te2e_tchr_cap        = 2048U,   /**< Per-bubble `TCHR` charset container capacity.    */
  k_te2e_logits_cap      = 2048U,   /**< Per-bubble quantized `int8_t` CTC logits buffer. */
  k_te2e_tvoc_cap        = 4096U,   /**< Per-bubble `TVOC` vocabulary container capacity. */
  k_te2e_tmnt_cap        = 65536U,  /**< Per-bubble `TMNT` int8 model container capacity. */
  k_te2e_fixture_bubbles = 3U,      /**< Bubbles across the 2-page deterministic chapter. */
  k_te2e_fixture_pages   = 2U,      /**< Pages in the deterministic chapter suite.        */
  k_te2e_max_det_comps   = 32U,     /**< Detector component cap per page.                 */
} translate_e2e_limits_t;

/** @brief `TPAK` v1 wire format constants matching `tools/translate_eval/src/tpak.zig`. */
typedef enum : uint32_t {
  k_te2e_tpak_magic        = 0x4B415054U, /**< Little-endian `"TPAK"`.            */
  k_te2e_tpak_version      = 1U,          /**< Supported `TPAK` version.          */
  k_te2e_tpak_header_bytes = 20U,         /**< Fixed wire header size.            */
  k_te2e_tpak_page_bytes   = 64U,         /**< Fixed wire page entry size.        */
  k_te2e_tpak_bubble_bytes = 44U,         /**< Fixed wire bubble entry size.      */
  k_te2e_tpak_page_id_cap  = 32U,         /**< Inline page identifier buffer cap. */
} translate_e2e_tpak_wire_t;

/** @brief Byte emitter callback used for streamed `EMU_TRACE_*` JSON output. */
typedef void (*translate_e2e_emit_fn)(void* ctx, const char* bytes, size_t len);

/** @brief Container set for one synthetic bubble (OCR `TCHR`+logits and NMT `TVOC`+`TMNT`). */
typedef struct {
  const char* src_utf8;
  const char* masked_src_utf8;
  const char* eng_utf8;
  uint8_t     tchr_buf[k_te2e_tchr_cap];
  size_t      tchr_len;
  int8_t      logits_i8[k_te2e_logits_cap];
  size_t      logits_len;
  uint32_t    time_steps;
  float       ocr_scale;
  int8_t      ocr_zp;
  uint8_t     tvoc_buf[k_te2e_tvoc_cap];
  size_t      tvoc_len;
  uint8_t     tmnt_buf[k_te2e_tmnt_cap];
  size_t      tmnt_len;
} translate_e2e_bubble_pack_t;

/** @brief Page detector fixture metadata for the 2-page Japanese golden chapter. */
typedef struct {
  uint16_t page_w;
  uint16_t page_h;
  uint16_t map_w;
  uint16_t map_h;
  int8_t   det_map[k_te2e_det_map_cap];
  size_t   det_elems;
  float    det_scale;
  int8_t   det_zp;
  uint8_t  bubble_count;
  uint8_t  first_bubble;
} translate_e2e_page_pack_t;

/** @brief Caller-owned scratch arena holding all work buffers and binary fixtures. */
typedef struct {
  uint8_t                     job_work[k_te2e_job_work_cap];
  uint8_t                     model_work[k_te2e_model_work_cap];
  uint8_t                     ckpt_snap[k_te2e_ckpt_cap];
  size_t                      ckpt_snap_len;
  uint8_t                     trn1_clean[k_te2e_trn1_cap];
  size_t                      trn1_clean_len;
  uint8_t                     trn1_resume[k_te2e_trn1_cap];
  size_t                      trn1_resume_len;
  uint8_t                     tgl1_out[k_te2e_tgl1_cap];
  size_t                      tgl1_out_len;
  translate_e2e_page_pack_t   pages[k_te2e_fixture_pages];
  translate_e2e_bubble_pack_t bubbles[k_te2e_fixture_bubbles];
  uint16_t                    capture_after_ckpt_seq;
  uint32_t                    furigana_dropped_total;
  uint64_t                    weight_bytes_read;
  uint32_t                    term_calls;
  uint32_t                    batch_calls;
} translate_e2e_arena_t;

/** @brief Observable summary returned by ::translate_e2e_run_chapter_suite. */
typedef struct {
  uint32_t bubble_count;
  uint32_t furigana_dropped;
  uint32_t trn1_bytes;
  uint32_t trn1_crc32;
  uint64_t weight_bytes_read;
  uint8_t  policy_verified;
  uint8_t  checkpoint_bit_exact;
} translate_e2e_chapter_report_t;

/** @brief Summary metrics produced by ::translate_e2e_run_tpak. */
typedef struct {
  uint32_t page_count;
  uint32_t bubble_count;
  uint32_t furigana_dropped;
  uint64_t weight_bytes_read;
} translate_e2e_tpak_stats_t;

/** @brief Compute standard IEEE 802.3 CRC-32 matching `libs/ra8_translate`. */
uint32_t translate_e2e_crc32(const uint8_t* bytes, size_t len);

/** @brief Compute IEEE CRC-32 treating `[zero_start, zero_start + zero_len)` as zeros. */
uint32_t
translate_e2e_crc32_zero_win(const uint8_t* bytes, size_t len, size_t zero_start, size_t zero_len);

/**
 * @brief Populate the deterministic 2-page Japanese golden chapter fixtures inside @p arena.
 * @return ::k_ra8_ok on success or explicit `ra8_err_t`.
 */
[[nodiscard]] ra8_err_t translate_e2e_populate_chapter_fixtures(translate_e2e_arena_t* arena);

/**
 * @brief Build a small self-contained 1-page Japanese `TPAK` v1 pack for smoke/unit runs.
 * @return ::k_ra8_ok on success or `k_ra8_err_invalid_size` / error code.
 */
[[nodiscard]] ra8_err_t translate_e2e_build_mini_tpak(translate_e2e_page_pack_t*   pg_scratch,
                                                      translate_e2e_bubble_pack_t* bp_scratch,
                                                      uint8_t*                     out_buf,
                                                      size_t                       out_cap,
                                                      size_t*                      out_len);

/**
 * @brief Execute policy gating, full chapter pipeline, and `TRJC` reset/restore parity.
 * @return ::k_ra8_ok if every stage, policy transition, and bit-exact restore passes.
 */
[[nodiscard]] ra8_err_t translate_e2e_run_chapter_suite(translate_e2e_arena_t*          arena,
                                                        translate_e2e_chapter_report_t* out_report);

/**
 * @brief Validate a `TPAK` v1 buffer, execute det->OCR->NMT per page, and emit JSON trace.
 * @return ::k_ra8_ok on valid container and successful inference, else error before PASS.
 */
[[nodiscard]] ra8_err_t translate_e2e_run_tpak(const uint8_t*              tpak_buf,
                                               size_t                      tpak_len,
                                               uint8_t*                    model_work,
                                               size_t                      model_work_cap,
                                               translate_e2e_emit_fn       emit_fn,
                                               void*                       emit_ctx,
                                               translate_e2e_tpak_stats_t* out_stats);

/**
 * @brief Attempt to initialize SDHI card, mount FAT filesystem, and read `TPAK.BIN`.
 * @retval k_ra8_ok Loaded `/TPAK.BIN` cleanly into @p out_buf.
 * @retval k_ra8_err_not_found No SD card present or `/TPAK.BIN` absent (non-fatal standalone).
 * @retval Other `ra8_err_t` Hardware or filesystem I/O failure while reading present file.
 */
[[nodiscard]] ra8_err_t
translate_e2e_sd_load_tpak(uint8_t* out_buf, size_t out_cap, size_t* out_len);

#ifdef __cplusplus
}
#endif
