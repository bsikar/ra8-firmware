/**
 * @file test_ra8_translate_marian.c
 * @brief Host C23 ABI acceptance tests for `libs/ra8_translate` int8 Marian NMT runtime.
 *
 * Builds a deterministic `TMNT` container with IEEE CRC-32 checksums, exercises
 * `ra8_translate_marian_open()`, `ra8_translate_marian_work_bytes()`,
 * `ra8_translate_marian_step_logits()`, and `ra8_translate_marian_greedy_decode()`,
 * checking greedy/step-logits parity, EOS early exit, and fail-closed guards.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "ra8_translate.h"
#include "unity_minimal.h"

static uint32_t ieee_crc32(const uint8_t* data, size_t len)
{
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (uint32_t b = 0; b < 8U; ++b) {
      const uint32_t mask = (crc & 1U) ? 0xEDB88320U : 0U;
      crc                 = (crc >> 1U) ^ mask;
    }
  }
  return ~crc;
}

static void wr_u16(uint8_t* buf, size_t off, uint16_t v)
{
  buf[off + 0U] = (uint8_t)(v & 0xFFU);
  buf[off + 1U] = (uint8_t)((v >> 8U) & 0xFFU);
}

static void wr_u32(uint8_t* buf, size_t off, uint32_t v)
{
  buf[off + 0U] = (uint8_t)(v & 0xFFU);
  buf[off + 1U] = (uint8_t)((v >> 8U) & 0xFFU);
  buf[off + 2U] = (uint8_t)((v >> 16U) & 0xFFU);
  buf[off + 3U] = (uint8_t)((v >> 24U) & 0xFFU);
}

static void wr_f32(uint8_t* buf, size_t off, float v)
{
  uint32_t bits = 0U;
  memcpy(&bits, &v, sizeof(bits));
  wr_u32(buf, off, bits);
}

typedef struct {
  uint8_t  dtype;
  uint32_t rows;
  uint32_t cols;
  float    scale;
  int8_t   zp;
  int8_t   fill_i8;
  float    fill_f32;
} c_tensor_spec_t;

static size_t build_test_tmnt(uint8_t* out, size_t cap, int force_tok)
{
  enum : uint32_t {
    k_vocab      = 8U,
    k_d          = 4U,
    k_ffn        = 8U,
    k_max_seq    = 8U,
    k_slots      = 8U + 16U + 26U,
    k_header     = k_ra8_translate_marian_header_bytes,
    k_desc       = k_ra8_translate_marian_tensor_desc_bytes,
    k_weights_at = ((k_header + k_slots * k_desc) + 3U) & ~3U,
  };
  c_tensor_spec_t specs[k_slots] = {};
  specs[0]                       = (c_tensor_spec_t){.dtype   = k_ra8_translate_marian_dtype_i8,
                                                     .rows    = k_vocab,
                                                     .cols    = k_d,
                                                     .scale   = 0.05F,
                                                     .zp      = 0,
                                                     .fill_i8 = 20};
  specs[2]                       = (c_tensor_spec_t){.dtype   = k_ra8_translate_marian_dtype_i8,
                                                     .rows    = k_vocab,
                                                     .cols    = k_d,
                                                     .scale   = 0.05F,
                                                     .zp      = 0,
                                                     .fill_i8 = 1};
  specs[3]                       = (c_tensor_spec_t){.dtype    = k_ra8_translate_marian_dtype_f32,
                                                     .rows     = k_vocab,
                                                     .cols     = 1U,
                                                     .scale    = 1.0F,
                                                     .fill_f32 = -5.0F};

  /* Encoder layer (slots 8..23): Q,K,V,O (8..15), attn_ln (16..17), fc1,fc2 (18..21), ffn_ln (22..23). */
  for (uint32_t p = 0U; p < 4U; ++p) {
    specs[8U + p * 2U] = (c_tensor_spec_t){.dtype   = k_ra8_translate_marian_dtype_i8,
                                           .rows    = k_d,
                                           .cols    = k_d,
                                           .scale   = 0.02F,
                                           .zp      = 0,
                                           .fill_i8 = 6};
  }
  specs[16] = (c_tensor_spec_t){.dtype    = k_ra8_translate_marian_dtype_f32,
                                .rows     = k_d,
                                .cols     = 1U,
                                .scale    = 1.0F,
                                .fill_f32 = 1.0F};
  specs[17] = (c_tensor_spec_t){.dtype    = k_ra8_translate_marian_dtype_f32,
                                .rows     = k_d,
                                .cols     = 1U,
                                .scale    = 1.0F,
                                .fill_f32 = 0.0F};
  specs[18] = (c_tensor_spec_t){.dtype   = k_ra8_translate_marian_dtype_i8,
                                .rows    = k_ffn,
                                .cols    = k_d,
                                .scale   = 0.02F,
                                .zp      = 0,
                                .fill_i8 = 5};
  specs[20] = (c_tensor_spec_t){.dtype   = k_ra8_translate_marian_dtype_i8,
                                .rows    = k_d,
                                .cols    = k_ffn,
                                .scale   = 0.02F,
                                .zp      = 0,
                                .fill_i8 = 4};
  specs[22] = specs[16];
  specs[23] = specs[17];

  /* Decoder layer (slots 24..49): self (24..33), cross (34..43), ffn (44..49). */
  for (uint32_t blk = 0U; blk < 2U; ++blk) {
    const uint32_t b = 24U + blk * 10U;
    for (uint32_t p = 0U; p < 4U; ++p) {
      specs[b + p * 2U] = (c_tensor_spec_t){.dtype   = k_ra8_translate_marian_dtype_i8,
                                            .rows    = k_d,
                                            .cols    = k_d,
                                            .scale   = 0.02F,
                                            .zp      = 0,
                                            .fill_i8 = 7};
    }
    specs[b + 8U] = specs[16];
    specs[b + 9U] = specs[17];
  }
  specs[44] = specs[18];
  specs[46] = specs[20];
  specs[48] = specs[16];
  specs[49] = specs[17];

  memset(out, 0, cap);
  size_t cursor = k_weights_at;
  for (uint32_t s = 0U; s < k_slots; ++s) {
    const size_t doff = k_header + (size_t)s * k_desc;
    if (specs[s].dtype == k_ra8_translate_marian_dtype_none) {
      wr_f32(out, doff + 16U, 1.0F);
      continue;
    }
    cursor         = (cursor + 3U) & ~3U;
    size_t   elems = (size_t)specs[s].rows * specs[s].cols;
    uint32_t bytes =
      (uint32_t)(elems * (specs[s].dtype == k_ra8_translate_marian_dtype_i8 ? 1U : 4U));
    TEST_ASSERT(cursor + bytes <= cap);
    if (specs[s].dtype == k_ra8_translate_marian_dtype_i8) {
      for (size_t k = 0; k < elems; ++k) {
        const int8_t v  = (int8_t)(specs[s].fill_i8 + (int8_t)((k * 5U) % 13U) - 6);
        out[cursor + k] = (uint8_t)v;
      }
    } else {
      for (size_t k = 0; k < elems; ++k) {
        float fv = specs[s].fill_f32;
        if (s == 3U && k == (size_t)force_tok) {
          fv = 20.0F;
        }
        wr_f32(out, cursor + k * 4U, fv);
      }
    }
    wr_u32(out, doff + 0U, (uint32_t)cursor);
    wr_u32(out, doff + 4U, bytes);
    wr_u32(out, doff + 8U, specs[s].rows);
    wr_u32(out, doff + 12U, specs[s].cols);
    wr_f32(out, doff + 16U, specs[s].scale);
    out[doff + 20U] = specs[s].dtype;
    out[doff + 21U] = (uint8_t)specs[s].zp;
    cursor += bytes;
  }

  const size_t total_bytes   = (cursor + 3U) & ~3U;
  const size_t dir_end       = k_header + (size_t)k_slots * k_desc;
  const size_t weights_bytes = total_bytes - k_weights_at;
  wr_u32(out, 0U, k_ra8_translate_marian_magic);
  wr_u16(out, 4U, (uint16_t)k_ra8_translate_marian_version);
  wr_u16(out, 6U, k_ra8_translate_marian_flag_scale_embed);
  wr_u32(out, 8U, (uint32_t)total_bytes);
  out[20] = (uint8_t)k_ra8_translate_nmt_ja_en;
  out[21] = 2U;
  out[22] = 1U;
  out[23] = 1U;
  wr_u16(out, 24U, (uint16_t)k_d);
  wr_u16(out, 26U, (uint16_t)k_ffn);
  wr_u16(out, 28U, (uint16_t)k_max_seq);
  wr_u32(out, 32U, k_vocab);
  wr_u32(out, 36U, 0U);
  wr_u32(out, 40U, 1U);
  wr_u32(out, 44U, 2U);
  wr_u32(out, 48U, 3U);
  wr_f32(out, 52U, 1e-5F);
  wr_u32(out, 72U, k_slots);
  wr_u32(out, 76U, k_weights_at);
  wr_u32(out, 80U, (uint32_t)weights_bytes);

  const uint32_t payload_crc = ieee_crc32(out + k_weights_at, total_bytes - k_weights_at);
  wr_u32(out, 16U, payload_crc);
  const uint32_t meta_crc = ieee_crc32(out, dir_end);
  wr_u32(out, 12U, meta_crc);
  return total_bytes;
}

static void test_marian_open_and_error_guards(void)
{
  uint8_t      buf[4096] = {};
  const size_t len       = build_test_tmnt(buf, sizeof(buf), 5);

  ra8_translate_marian_header_t hdr = {.vocab_size = 777U};
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_translate_marian_open(nullptr, len, k_ra8_translate_marian_open_meta_only, &hdr));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_translate_marian_open(buf, len, k_ra8_translate_marian_open_meta_only, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_translate_marian_open(buf, len, 0x80U, &hdr));
  TEST_ASSERT_EQ(777U, hdr.vocab_size);

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_translate_marian_open(buf, len, k_ra8_translate_marian_open_verify_payload, &hdr));
  TEST_ASSERT_EQ(8U, hdr.vocab_size);
  TEST_ASSERT_EQ(2U, hdr.eos_id);
  TEST_ASSERT_EQ((uint8_t)k_ra8_translate_nmt_ja_en, hdr.nmt_model);

  /* Corrupting a weight byte triggers CrcMismatch only when payload verification is enabled. */
  buf[len - 3U] ^= 0x11U;
  hdr.vocab_size = 999U;
  TEST_ASSERT_EQ(
    k_ra8_err_crc_mismatch,
    ra8_translate_marian_open(buf, len, k_ra8_translate_marian_open_verify_payload, &hdr));
  TEST_ASSERT_EQ(999U, hdr.vocab_size);
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_translate_marian_open(buf, len, k_ra8_translate_marian_open_meta_only, &hdr));
}

static void test_marian_step_logits_and_greedy_decode(void)
{
  uint8_t      buf[4096] = {};
  const size_t len       = build_test_tmnt(buf, sizeof(buf), 5);

  size_t work_bytes = 0U;
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_translate_marian_work_bytes(buf, len, 4U, 4U, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_marian_work_bytes(buf, len, 4U, 4U, &work_bytes));
  TEST_ASSERT(work_bytes > 0U && work_bytes <= 8192U);

  uint8_t        work[8192] = {};
  const uint32_t src[3]     = {4U, 5U, 6U};
  const uint32_t prefix[1]  = {1U};
  float          logits[8]  = {};

  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_translate_marian_step_logits(buf, len, src, 3U, prefix, 1U, work, work_bytes, logits, 8U));
  size_t best0 = 0U;
  for (size_t i = 1U; i < 8U; ++i) {
    if (logits[i] > logits[best0]) {
      best0 = i;
    }
  }
  TEST_ASSERT_EQ(5U, (uint32_t)best0);

  uint32_t                     out_ids[4] = {99U, 99U, 99U, 99U};
  ra8_translate_marian_stats_t stats      = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_translate_marian_greedy_decode(buf,
                                                    len,
                                                    src,
                                                    3U,
                                                    3U,
                                                    work,
                                                    work_bytes,
                                                    out_ids,
                                                    4U,
                                                    &stats));
  TEST_ASSERT_EQ(5U, out_ids[0]);
  TEST_ASSERT_EQ(5U, out_ids[1]);
  TEST_ASSERT_EQ(5U, out_ids[2]);
  TEST_ASSERT_EQ(3U, stats.generated_tokens);
  TEST_ASSERT_EQ(0U, stats.stopped_on_eos);
  TEST_ASSERT(stats.weight_bytes_read > 0U);
  TEST_ASSERT_EQ(0U, stats.used_helium_mve);

  const uint32_t prefix2[2] = {1U, out_ids[0]};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_translate_marian_step_logits(buf, len, src, 3U, prefix2, 2U, work, work_bytes, logits, 8U));
  size_t best1 = 0U;
  for (size_t i = 1U; i < 8U; ++i) {
    if (logits[i] > logits[best1]) {
      best1 = i;
    }
  }
  TEST_ASSERT_EQ(out_ids[1], (uint32_t)best1);

  /* Early EOS termination case (`force_tok = eos_id = 2`). */
  uint8_t      eos_buf[4096] = {};
  const size_t eos_len       = build_test_tmnt(eos_buf, sizeof(eos_buf), 2);
  uint32_t     eos_out[4]    = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_translate_marian_greedy_decode(eos_buf,
                                                    eos_len,
                                                    src,
                                                    3U,
                                                    4U,
                                                    work,
                                                    work_bytes,
                                                    eos_out,
                                                    4U,
                                                    &stats));
  TEST_ASSERT_EQ(2U, eos_out[0]);
  TEST_ASSERT_EQ(1U, stats.generated_tokens);
  TEST_ASSERT_EQ(1U, stats.stopped_on_eos);

  /* Fail-closed guard leaves outputs untouched on out-of-vocab token or short workspace. */
  const uint32_t bad_src[2] = {1U, 25U};
  out_ids[0]                = 1234U;
  stats.generated_tokens    = 88U;
  TEST_ASSERT_EQ(k_ra8_err_range_check_failed,
                 ra8_translate_marian_greedy_decode(buf,
                                                    len,
                                                    bad_src,
                                                    2U,
                                                    4U,
                                                    work,
                                                    work_bytes,
                                                    out_ids,
                                                    4U,
                                                    &stats));
  TEST_ASSERT_EQ(1234U, out_ids[0]);
  TEST_ASSERT_EQ(88U, stats.generated_tokens);

  size_t work_bytes_exact = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_marian_work_bytes(buf, len, 3U, 1U, &work_bytes_exact));
  logits[0] = -42.0F;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_translate_marian_step_logits(buf,
                                                  len,
                                                  src,
                                                  3U,
                                                  prefix,
                                                  1U,
                                                  work,
                                                  work_bytes_exact - 1U,
                                                  logits,
                                                  8U));
  TEST_ASSERT(fabsf(logits[0] + 42.0F) < 1e-6F);
}

int main(void)
{
  TEST_BEGIN("test_marian_open_and_error_guards");
  test_marian_open_and_error_guards();
  TEST_END("test_marian_open_and_error_guards");

  TEST_BEGIN("test_marian_step_logits_and_greedy_decode");
  test_marian_step_logits_and_greedy_decode();
  TEST_END("test_marian_step_logits_and_greedy_decode");
  return 0;
}
