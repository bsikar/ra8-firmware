/**
 * @file test_ra8_translate_ctc.c
 * @brief Host C23 ABI acceptance tests for `libs/ra8_translate` CTC decoding.
 *
 * Exercises all public `ra8_translate_ctc_*` C ABI exports declared in
 * `ra8_translate_ctc.h` against the `libra8_translate.a` archive.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_translate.h"
#include "unity_minimal.h"

static void ctc_wr_u16(uint8_t* p, uint16_t v)
{
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)((v >> 8U) & 0xFFU);
}

static void ctc_wr_u32(uint8_t* p, uint32_t v)
{
  p[0] = (uint8_t)(v & 0xFFU);
  p[1] = (uint8_t)((v >> 8U) & 0xFFU);
  p[2] = (uint8_t)((v >> 16U) & 0xFFU);
  p[3] = (uint8_t)((v >> 24U) & 0xFFU);
}

static uint32_t ctc_crc32_ieee(const uint8_t* data, size_t len)
{
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0U; i < len; ++i) {
    crc ^= data[i];
    for (uint32_t b = 0U; b < 8U; ++b) {
      uint32_t mask = (crc & 1U) ? 0xEDB88320U : 0U;
      crc           = (crc >> 1U) ^ mask;
    }
  }
  return ~crc;
}

static size_t build_test_tchr(ra8_translate_ocr_model_t ocr_model,
                              uint32_t                  blank_idx,
                              uint32_t                  unk_idx,
                              const char* const*        glyphs,
                              uint32_t                  count,
                              uint8_t*                  out_buf)
{
  const size_t hdr_bytes     = k_ra8_translate_ctc_header_bytes;
  const size_t offsets_bytes = ((size_t)count + 1U) * 4U;
  const size_t pool_off      = hdr_bytes + offsets_bytes;
  size_t       pool_cursor   = 0U;

  ctc_wr_u32(out_buf + hdr_bytes, 0U);
  for (uint32_t i = 0U; i < count; ++i) {
    size_t len = strlen(glyphs[i]);
    if (len > 0U) {
      memcpy(out_buf + pool_off + pool_cursor, glyphs[i], len);
    }
    pool_cursor += len;
    ctc_wr_u32(out_buf + hdr_bytes + ((size_t)i + 1U) * 4U, (uint32_t)pool_cursor);
  }

  size_t total = pool_off + pool_cursor;
  ctc_wr_u32(out_buf + 0U, k_ra8_translate_ctc_magic);
  ctc_wr_u16(out_buf + 4U, k_ra8_translate_ctc_version);
  ctc_wr_u16(out_buf + 6U, 0U);
  ctc_wr_u32(out_buf + 8U, (uint32_t)total);
  ctc_wr_u32(out_buf + 12U, 0U);
  out_buf[16] = (uint8_t)ocr_model;
  out_buf[17] = 0U;
  out_buf[18] = 0U;
  out_buf[19] = 0U;
  ctc_wr_u32(out_buf + 20U, count);
  ctc_wr_u32(out_buf + 24U, blank_idx);
  ctc_wr_u32(out_buf + 28U, unk_idx);
  ctc_wr_u32(out_buf + 32U, (uint32_t)pool_cursor);
  ctc_wr_u32(out_buf + 12U, ctc_crc32_ieee(out_buf, total));
  return total;
}

static void test_ctc_korean_and_errors(void)
{
  const char* const ko_glyphs[]  = {"", "\xec\x95\x88", "\xeb\x85\x95", "!"};
  uint8_t           ko_tchr[256] = {};
  const size_t      ko_len       = build_test_tchr(k_ra8_translate_ocr_korean,
                                                   0U,
                                                   k_ra8_translate_ctc_index_none,
                                                   ko_glyphs,
                                                   4U,
                                                   ko_tchr);

  ra8_translate_ctc_charset_header_t hdr = {.class_count = 99U};
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_translate_ctc_charset_open(nullptr, ko_len, &hdr));
  TEST_ASSERT_EQ(99U, hdr.class_count);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_ctc_charset_open(ko_tchr, ko_len, &hdr));
  TEST_ASSERT_EQ(4U, hdr.class_count);
  TEST_ASSERT_EQ((uint8_t)k_ra8_translate_ocr_korean, hdr.ocr_model);

  const uint8_t* g_ptr = nullptr;
  uint16_t       g_len = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_translate_ctc_charset_glyph(ko_tchr, ko_len, 2U, &g_ptr, &g_len));
  TEST_ASSERT_EQ(3U, g_len);
  TEST_ASSERT_EQ(0, memcmp(g_ptr, "\xeb\x85\x95", 3U));

  /* Path: [glyph1, glyph1, blank, glyph1, glyph2, glyph3] -> an-an-nyeong! */
  const int8_t logits_i8[6U * 4U] = {
    -20, 20, -20, -20, -20, 24,  -20, -20, 20,  -20, -20, -20,
    -20, 22, -20, -20, -20, -20, 22,  -20, -20, -20, -20, 22,
  };
  const ra8_translate_ctc_decode_cfg_t cfg = {
    .time_steps                = 6U,
    .class_stride              = 0U,
    .score_mode                = k_ra8_translate_ctc_score_logits,
    .decode_flags              = k_ra8_translate_ctc_flag_none,
    .low_confidence_thresh_bps = k_ra8_translate_ctc_default_low_conf_bps,
    .expected_ocr_model        = (uint8_t)k_ra8_translate_ocr_korean,
    .reserved                  = {0U, 0U, 0U},
  };
  uint8_t                    out[32] = {};
  ra8_translate_ctc_result_t res     = {};
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_translate_ctc_decode_i8(ko_tchr,
                                             ko_len,
                                             logits_i8,
                                             sizeof(logits_i8),
                                             0.25F,
                                             0,
                                             &cfg,
                                             out,
                                             sizeof(out),
                                             &res));
  const char want_ko[] = "\xec\x95\x88\xec\x95\x88\xeb\x85\x95!";
  TEST_ASSERT_EQ(sizeof(want_ko) - 1U, res.text_bytes);
  TEST_ASSERT_EQ(0, memcmp(out, want_ko, res.text_bytes));
  TEST_ASSERT_EQ(4U, res.emitted_tokens);
  TEST_ASSERT_EQ(1U, res.blank_frames);
  TEST_ASSERT(res.confidence_bps >= 9900U);
  TEST_ASSERT_EQ(0U, res.low_confidence);

  /* Buffer-too-small and CRC corruption must not touch caller outputs. */
  uint8_t guard[4] = {0x11U, 0x22U, 0x33U, 0x44U};
  res.text_bytes   = 99U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_size,
                 ra8_translate_ctc_decode_i8(ko_tchr,
                                             ko_len,
                                             logits_i8,
                                             sizeof(logits_i8),
                                             0.25F,
                                             0,
                                             &cfg,
                                             guard,
                                             4U,
                                             &res));
  TEST_ASSERT_EQ(0x11U, guard[0]);
  TEST_ASSERT_EQ(99U, res.text_bytes);

  ko_tchr[ko_len - 1U] ^= 0x01U;
  TEST_ASSERT_EQ(k_ra8_err_crc_mismatch, ra8_translate_ctc_charset_open(ko_tchr, ko_len, &hdr));
}

static void test_ctc_ja_zh_probs_and_low_conf(void)
{
  const char* const ja_glyphs[]  = {"\xe3\x81\xaf", "?", ""};
  uint8_t           ja_tchr[256] = {};
  const size_t      ja_len =
    build_test_tchr(k_ra8_translate_ocr_japanese, 2U, 1U, ja_glyphs, 3U, ja_tchr);

  const float logits[3U * 3U] = {
    0.05F,
    0.20F,
    0.00F,
    0.00F,
    0.00F,
    5.00F,
    0.30F,
    0.10F,
    0.10F,
  };
  const ra8_translate_ctc_decode_cfg_t skip_cfg = {
    .time_steps                = 3U,
    .class_stride              = 0U,
    .score_mode                = k_ra8_translate_ctc_score_logits,
    .decode_flags              = k_ra8_translate_ctc_flag_skip_unk,
    .low_confidence_thresh_bps = k_ra8_translate_ctc_default_low_conf_bps,
    .expected_ocr_model        = (uint8_t)k_ra8_translate_ocr_japanese,
    .reserved                  = {0U, 0U, 0U},
  };
  uint8_t                    out[16] = {};
  ra8_translate_ctc_result_t res     = {};
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_translate_ctc_decode_f32(ja_tchr, ja_len, logits, 9U, &skip_cfg, out, sizeof(out), &res));
  TEST_ASSERT_EQ(3U, res.text_bytes);
  TEST_ASSERT_EQ(0, memcmp(out, "\xe3\x81\xaf", 3U));
  TEST_ASSERT_EQ(1U, res.unk_count);
  TEST_ASSERT_EQ(1U, res.low_confidence);
  TEST_ASSERT(res.confidence_bps < k_ra8_translate_ctc_default_low_conf_bps);

  /* Chinese probability mode with script mismatch guard check. */
  const char* const              zh_glyphs[]  = {"", "\xe4\xb8\xad", "\xe6\x96\x87"};
  uint8_t                        zh_tchr[256] = {};
  const size_t                   zh_len       = build_test_tchr(k_ra8_translate_ocr_chinese,
                                                                0U,
                                                                k_ra8_translate_ctc_index_none,
                                                                zh_glyphs,
                                                                3U,
                                                                zh_tchr);
  const float                    zh_probs[6]  = {0.02F, 0.90F, 0.08F, 0.05F, 0.05F, 0.90F};
  ra8_translate_ctc_decode_cfg_t zh_cfg       = {
    .time_steps                = 2U,
    .class_stride              = 0U,
    .score_mode                = k_ra8_translate_ctc_score_probs,
    .decode_flags              = k_ra8_translate_ctc_flag_none,
    .low_confidence_thresh_bps = 8000U,
    .expected_ocr_model        = (uint8_t)k_ra8_translate_ocr_korean,
    .reserved                  = {0U, 0U, 0U},
  };
  TEST_ASSERT_EQ(
    k_ra8_err_validation_failed,
    ra8_translate_ctc_decode_f32(zh_tchr, zh_len, zh_probs, 6U, &zh_cfg, out, sizeof(out), &res));

  zh_cfg.expected_ocr_model = (uint8_t)k_ra8_translate_ocr_chinese;
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_translate_ctc_decode_f32(zh_tchr, zh_len, zh_probs, 6U, &zh_cfg, out, sizeof(out), &res));
  TEST_ASSERT_EQ(6U, res.text_bytes);
  TEST_ASSERT_EQ(0, memcmp(out, "\xe4\xb8\xad\xe6\x96\x87", 6U));
  TEST_ASSERT_EQ(9000U, res.confidence_bps);
  TEST_ASSERT_EQ(0U, res.low_confidence);
}

int main(void)
{
  TEST_BEGIN("test_ctc_korean_and_errors");
  test_ctc_korean_and_errors();
  TEST_END("test_ctc_korean_and_errors");

  TEST_BEGIN("test_ctc_ja_zh_probs_and_low_conf");
  test_ctc_ja_zh_probs_and_low_conf();
  TEST_END("test_ctc_ja_zh_probs_and_low_conf");
  return 0;
}
